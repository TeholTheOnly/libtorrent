/*

Copyright (c) 2025, libtorrent project
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/config.hpp"

#ifdef TORRENT_USE_LIBCURL

#include "libtorrent/aux_/curl_tracker_manager.hpp"
#include "libtorrent/http2_errors.hpp"
#include "libtorrent/aux_/time.hpp"
#include <chrono>
#include <mutex>
#include <atomic>

#ifndef TORRENT_WINDOWS
#include <unistd.h>  // For dup() and close()
#endif

namespace libtorrent { namespace aux {

namespace {
	// Global initialization guard for curl
	std::once_flag curl_init_flag;
	std::atomic<int> curl_init_count{0};

	void ensure_curl_initialized() {
		std::call_once(curl_init_flag, []() {
			// Initialize curl globally with SSL support
			curl_global_init(CURL_GLOBAL_ALL);
			curl_init_count++;
		});
	}

	// Write callback for curl
	size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
		auto* response = static_cast<std::vector<char>*>(userdata);
		size_t total = size * nmemb;
		response->insert(response->end(), ptr, ptr + total);
		return total;
	}

	// Map curl error codes to libtorrent error codes
	error_code curl_error_to_libtorrent(CURLcode code) {
		switch(code) {
			case CURLE_OK:
				return {};
			case CURLE_OPERATION_TIMEDOUT:
				return errors::timed_out;
			case CURLE_COULDNT_CONNECT:
				// Use a generic HTTP error for connection issues
				return errors::http_error;
			case CURLE_COULDNT_RESOLVE_HOST:
				return errors::invalid_hostname;
			case CURLE_URL_MALFORMAT:
				return errors::url_parse_error;
			case CURLE_TOO_MANY_REDIRECTS:
				// Use generic HTTP error for too many redirects
				return errors::http_error;
			case CURLE_SSL_CONNECT_ERROR:
			case CURLE_SSL_CERTPROBLEM:
			case CURLE_SSL_CIPHER:
			case CURLE_SSL_CACERT:
				// Use generic HTTP error for SSL issues
				return errors::http_error;
			case CURLE_OUT_OF_MEMORY:
				return errors::no_memory;
			default:
				return errors::http_error;
		}
	}
}

curl_tracker_manager::curl_tracker_manager(io_context& ios, settings_pack const& settings)
	: m_ios(ios)
	, m_multi(nullptr)
	, m_share(nullptr)
	, m_timer(ios)
	, m_still_running(0)
	, m_settings(settings)
	, m_http2_supported(false)
	, m_shutting_down(false)
{
	// Ensure curl is initialized globally
	ensure_curl_initialized();

	// Initialize curl multi handle
	m_multi = curl_multi_init();
	if (!m_multi) {
		throw std::runtime_error("Failed to initialize curl multi handle");
	}

	// Set socket callback - integrates with Boost.Asio
	curl_multi_setopt(m_multi, CURLMOPT_SOCKETFUNCTION, &socket_callback);
	curl_multi_setopt(m_multi, CURLMOPT_SOCKETDATA, this);

	// Set timer callback - manages timeouts
	curl_multi_setopt(m_multi, CURLMOPT_TIMERFUNCTION, &timer_callback);
	curl_multi_setopt(m_multi, CURLMOPT_TIMERDATA, this);

	// Enable HTTP/2 multiplexing if available
	#ifdef CURLPIPE_MULTIPLEX
	curl_multi_setopt(m_multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
	#endif

	// Set connection limits
	long max_total = settings.get_int(settings_pack::connections_limit);
	if (max_total > 0) {
		curl_multi_setopt(m_multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, max_total);
	}

	long max_per_host = settings.get_int(settings_pack::connections_limit) / 10; // reasonable default
	if (max_per_host > 0) {
		curl_multi_setopt(m_multi, CURLMOPT_MAX_HOST_CONNECTIONS, max_per_host);
	}

	// Initialize share handle for connection pooling
	m_share = curl_share_init();
	if (m_share) {
		curl_share_setopt(m_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
		curl_share_setopt(m_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
		curl_share_setopt(m_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	}

	// Check for HTTP/2 support
	curl_version_info_data* ver = curl_version_info(CURLVERSION_NOW);
	m_http2_supported = (ver && (ver->features & CURL_VERSION_HTTP2));
}

curl_tracker_manager::~curl_tracker_manager() {
	// Set shutdown flag to prevent new callbacks
	m_shutting_down = true;

	// Cancel timer to prevent callbacks after destruction
	m_timer.cancel();

	// Clean up all sockets first to prevent callbacks
	for (auto& pair : m_sockets) {
		pair.second->monitor.close();
	}
	m_sockets.clear();

	// Clean up all requests
	for (auto& pair : m_requests) {
		CURL* easy = pair.first;
		curl_multi_remove_handle(m_multi, easy);
		curl_easy_cleanup(easy);
	}
	m_requests.clear();

	if (m_share) curl_share_cleanup(m_share);
	if (m_multi) curl_multi_cleanup(m_multi);
}

bool curl_tracker_manager::supports_http2() const {
	return m_http2_supported;
}

// Socket callback - called by curl when it wants to monitor a socket
int curl_tracker_manager::socket_callback(CURL* easy, curl_socket_t sock, int what,
                                         void* userp, void* socketp) {
	TORRENT_UNUSED(easy);
	TORRENT_UNUSED(socketp);
	auto* manager = static_cast<curl_tracker_manager*>(userp);

	// Don't process callbacks if we're shutting down
	if (manager->m_shutting_down) {
		return 0;
	}

	if (what == CURL_POLL_REMOVE) {
		// Stop monitoring this socket
		auto it = manager->m_sockets.find(sock);
		if (it != manager->m_sockets.end()) {
			// Mark as not wanted
			it->second->wants_read = false;
			it->second->wants_write = false;
			// Cancel any pending operations
			it->second->monitor.cancel();
#ifdef TORRENT_WINDOWS
			// Windows: Release the socket to prevent closing curl's fd
			// tcp::socket took ownership of curl's fd, so we must release it
			it->second->monitor.release();
#else
			// POSIX: Let destructor close our duplicated fd
			// We're using a dup()'ed fd, so we can safely close it
#endif
			manager->m_sockets.erase(it);
		}
		return 0;
	}

	// Find or create socket info
	auto it = manager->m_sockets.find(sock);
	if (it == manager->m_sockets.end()) {
		auto info = std::make_shared<socket_info>(manager->m_ios);
		boost::system::error_code ec;

#ifdef TORRENT_WINDOWS
		// Windows: tcp::socket takes ownership of the fd
		// We must call release() in CURL_POLL_REMOVE to prevent double-close
		info->monitor.assign(tcp::v4(), sock, ec);
		if (ec) {
			info->monitor.assign(tcp::v6(), sock, ec);
			if (ec) {
				return -1;
			}
		}
#else
		// POSIX: Duplicate the fd so we can monitor our own copy
		// curl retains ownership of the original fd
		int dup_fd = ::dup(sock);
		if (dup_fd == -1) {
			// Failed to duplicate the file descriptor
			return -1;
		}
		info->monitor.assign(dup_fd, ec);
		if (ec) {
			// Clean up the duplicated fd on failure
			::close(dup_fd);
			return -1;
		}
		// Now stream_descriptor owns dup_fd, curl still owns sock
#endif

		it = manager->m_sockets.emplace(sock, info).first;

		// Associate the socket info with this socket for curl
		curl_multi_assign(manager->m_multi, sock, it->second.get());
	}

	// Use the new pattern - just call monitor_socket
	auto info_ptr = it->second;
	auto& info = *info_ptr;
	manager->monitor_socket(sock, info, what);

	return 0;
}

// Timer callback - called by curl to set timeout
int curl_tracker_manager::timer_callback(CURLM* multi, long timeout_ms, void* userp) {
	TORRENT_UNUSED(multi);
	auto* manager = static_cast<curl_tracker_manager*>(userp);

	manager->m_timer.cancel();

	if (timeout_ms < 0) {
		// No timeout needed
		return 0;
	}

	auto weak_manager = std::weak_ptr<curl_tracker_manager>(manager->shared_from_this());

	// Special case: 0 means call socket_action immediately
	if (timeout_ms == 0) {
		// Post immediately to the io_context
		boost::asio::post(manager->m_ios, [weak_manager]() {
			if (auto mgr = weak_manager.lock()) {
				if (!mgr->m_shutting_down) {
					mgr->socket_action(CURL_SOCKET_TIMEOUT, 0);
				}
			}
		});
		return 0;
	}

	// Set timer for the specified timeout
	manager->m_timer.expires_after(milliseconds(timeout_ms));
	manager->m_timer.async_wait(
		[weak_manager](error_code const& ec) {
			if (auto mgr = weak_manager.lock()) {
				if (!ec && !mgr->m_shutting_down) {
					mgr->timer_expired(ec);
				}
			}
		});

	return 0;
}

void curl_tracker_manager::configure_security(CURL* easy)
{
	// SSL/TLS verification
	bool verify_peer = m_settings.get_bool(settings_pack::tracker_ssl_verify_peer);
	bool verify_host = m_settings.get_bool(settings_pack::tracker_ssl_verify_host);

	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, verify_peer ? 1L : 0L);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, verify_host ? 2L : 0L);

	// Set minimum TLS version
	int min_tls = m_settings.get_int(settings_pack::tracker_min_tls_version);
	long tls_version = CURL_SSLVERSION_TLSv1_2; // Default to TLS 1.2
	switch (min_tls) {
		case 0: tls_version = CURL_SSLVERSION_TLSv1_0; break;
		case 1: tls_version = CURL_SSLVERSION_TLSv1_1; break;
		case 2: tls_version = CURL_SSLVERSION_TLSv1_2; break;
#ifdef CURL_SSLVERSION_TLSv1_3
		case 3: tls_version = CURL_SSLVERSION_TLSv1_3; break;
#endif
		default: tls_version = CURL_SSLVERSION_TLSv1_2; break;
	}
	curl_easy_setopt(easy, CURLOPT_SSLVERSION, tls_version);

	// Maximum response size
	int64_t max_size = m_settings.get_int(settings_pack::tracker_max_response_size);
	if (max_size > 0) {
		curl_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(max_size));
	}

	// Redirects are not supported for security reasons
	curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);

	// Restrict protocols to HTTP and HTTPS only
#if CURL_AT_LEAST_VERSION(7, 85, 0)
	curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(easy, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

	// Enable DNS shuffling for load balancing
#ifdef CURLOPT_DNS_SHUFFLE_ADDRESSES
	curl_easy_setopt(easy, CURLOPT_DNS_SHUFFLE_ADDRESSES, 1L);
#endif

	// Set cipher list for better security
	curl_easy_setopt(easy, CURLOPT_SSL_CIPHER_LIST,
		"ECDHE-RSA-AES128-GCM-SHA256:"
		"ECDHE-ECDSA-AES128-GCM-SHA256:"
		"ECDHE-RSA-AES256-GCM-SHA384:"
		"ECDHE-ECDSA-AES256-GCM-SHA384:"
		"DHE-RSA-AES128-GCM-SHA256:"
		"DHE-DSS-AES128-GCM-SHA256:"
		"ECDHE-RSA-AES128-SHA256:"
		"ECDHE-ECDSA-AES128-SHA256:"
		"ECDHE-RSA-AES128-SHA:"
		"ECDHE-ECDSA-AES128-SHA:"
		"ECDHE-RSA-AES256-SHA384:"
		"ECDHE-ECDSA-AES256-SHA384:"
		"ECDHE-RSA-AES256-SHA:"
		"ECDHE-ECDSA-AES256-SHA:"
		"DHE-RSA-AES128-SHA256:"
		"DHE-RSA-AES256-SHA256:"
		"AES128-GCM-SHA256:"
		"AES256-GCM-SHA384:"
		"AES128-SHA256:"
		"AES256-SHA256:"
		"AES128-SHA:"
		"AES256-SHA:"
		"!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!aECDH:"
		"!EDH-DSS-DES-CBC3-SHA:!EDH-RSA-DES-CBC3-SHA:!KRB5-DES-CBC3-SHA");
}

void curl_tracker_manager::monitor_socket(curl_socket_t sock, socket_info& info, int what)
{
	if (m_shutting_down) return;
	
	// Update intent flags based on what curl wants
	info.wants_read = (what & CURL_POLL_IN);
	info.wants_write = (what & CURL_POLL_OUT);
	
	// Find the shared_ptr for this socket_info
	auto it = m_sockets.find(sock);
	if (it == m_sockets.end()) return;
	std::shared_ptr<socket_info> info_ptr = it->second;
	
	// Start monitoring loops if needed
	manage_io_loop(sock, info_ptr, CURL_POLL_IN);
	manage_io_loop(sock, info_ptr, CURL_POLL_OUT);
}

void curl_tracker_manager::manage_io_loop(curl_socket_t sock, 
                                          std::shared_ptr<socket_info> info, 
                                          int direction)
{
	// Determine state variables based on direction
	bool wants_io;
	bool& waiting_io = (direction == CURL_POLL_IN) ? 
	                   info->waiting_read : info->waiting_write;
	platform_socket_monitor::wait_type wait_type;
	int cselect_flag;

	if (direction == CURL_POLL_IN) {
		wants_io = info->wants_read;
		wait_type = platform_socket_monitor::wait_read;
		cselect_flag = CURL_CSELECT_IN;
	} else {
		wants_io = info->wants_write;
		wait_type = platform_socket_monitor::wait_write;
		cselect_flag = CURL_CSELECT_OUT;
	}

	// Check if already waiting or if I/O is not desired
	if (waiting_io || !wants_io) {
		return;
	}

	// Start the async_wait
	waiting_io = true;
	
	// Capture shared_ptrs for lifetime management
	std::weak_ptr<curl_tracker_manager> weak_manager = shared_from_this();

	info->monitor.async_wait(wait_type,
		[weak_manager, sock, info, direction, cselect_flag](error_code const& ec) {
			
			// Check manager lifetime
			auto mgr = weak_manager.lock();
			if (!mgr || mgr->m_shutting_down) return;

			// Update waiting state
			bool& current_waiting = (direction == CURL_POLL_IN) ? 
			                       info->waiting_read : info->waiting_write;
			current_waiting = false;

			// Handle errors
			if (ec) {
				if (ec != boost::asio::error::operation_aborted) {
					// Real error, notify curl
					mgr->socket_action(sock, CURL_CSELECT_ERR);
				}
				return; // Stop the loop on error
			}

			// Success - socket is ready
			// Notify curl (this may synchronously change state)
			mgr->socket_action(sock, cselect_flag);

			// Continue the loop asynchronously to prevent stack overflow
			// CRITICAL: Use boost::asio::post to ensure asynchronous continuation
			boost::asio::post(mgr->m_ios, [weak_manager, sock, info, direction]() {
				auto mgr_post = weak_manager.lock();
				if (!mgr_post || mgr_post->m_shutting_down) return;

				// Verify socket still exists before continuing
				if (mgr_post->m_sockets.count(sock) > 0) {
					mgr_post->manage_io_loop(sock, info, direction);
				}
			});
		});
}

CURL* curl_tracker_manager::add_request(
	std::string const& url,
	std::function<void(error_code const&, std::vector<char> const&)> handler)
{
	// Create curl easy handle
	CURL* easy = curl_easy_init();
	if (!easy) {
		handler(errors::no_memory, {});
		return nullptr;
	}

	// Create request info
	auto req = std::make_unique<request_info>();
	req->easy = easy;
	req->handler = handler;

	// Configure the request
	curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
	curl_easy_setopt(easy, CURLOPT_SHARE, m_share);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &req->response);
	curl_easy_setopt(easy, CURLOPT_PRIVATE, req.get());

	// Disable SIGPIPE signals (crucial for multi-threaded applications)
	// This prevents crashes when writing to closed sockets
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);

	// Enable HTTP/2 if available and configured
	if (m_http2_supported && m_settings.get_bool(settings_pack::enable_http2_trackers)) {
		curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
	}

	// Enable keep-alive
	curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE, 120L);
	curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL, 60L);

	// Ensure HTTP errors are properly reported
	curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L);

	// Set timeouts
	int connect_timeout = m_settings.get_int(settings_pack::tracker_completion_timeout);
	if (connect_timeout <= 0) connect_timeout = 30; // default 30 seconds
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, static_cast<long>(connect_timeout));

	int timeout = m_settings.get_int(settings_pack::tracker_receive_timeout);
	if (timeout <= 0) timeout = 60; // default 60 seconds
	curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(timeout));

	// Set User-Agent
	std::string user_agent = m_settings.get_str(settings_pack::user_agent);
	if (!user_agent.empty()) {
		curl_easy_setopt(easy, CURLOPT_USERAGENT, user_agent.c_str());
	}

	// Configure proxy settings if needed
	if (m_settings.get_bool(settings_pack::proxy_tracker_connections)) {
		int proxy_type = m_settings.get_int(settings_pack::proxy_type);
		if (proxy_type != settings_pack::none) {
			std::string proxy_host = m_settings.get_str(settings_pack::proxy_hostname);
			int proxy_port = m_settings.get_int(settings_pack::proxy_port);

			if (!proxy_host.empty() && proxy_port > 0) {
				// Set proxy URL
				std::string proxy_url = proxy_host + ":" + std::to_string(proxy_port);
				curl_easy_setopt(easy, CURLOPT_PROXY, proxy_url.c_str());

				// Prevent localhost bypass when proxy is configured
				// Empty string means no hosts bypass the proxy
				curl_easy_setopt(easy, CURLOPT_NOPROXY, "");

				// Set proxy type
				long curl_proxy_type = CURLPROXY_HTTP; // default
				switch (proxy_type) {
					case settings_pack::socks4:
						curl_proxy_type = CURLPROXY_SOCKS4;
						break;
					case settings_pack::socks5:
					case settings_pack::socks5_pw:
						curl_proxy_type = CURLPROXY_SOCKS5;
						break;
					case settings_pack::http:
					case settings_pack::http_pw:
						curl_proxy_type = CURLPROXY_HTTP;
						break;
				}
				curl_easy_setopt(easy, CURLOPT_PROXYTYPE, curl_proxy_type);

				// Set proxy authentication if needed
				if (proxy_type == settings_pack::socks5_pw ||
				    proxy_type == settings_pack::http_pw) {
					std::string proxy_user = m_settings.get_str(settings_pack::proxy_username);
					std::string proxy_pass = m_settings.get_str(settings_pack::proxy_password);
					if (!proxy_user.empty()) {
						curl_easy_setopt(easy, CURLOPT_PROXYUSERNAME, proxy_user.c_str());
						curl_easy_setopt(easy, CURLOPT_PROXYPASSWORD, proxy_pass.c_str());
					}
				}

				// Enable proxy hostname resolution if configured
				if (m_settings.get_bool(settings_pack::proxy_hostnames)) {
					curl_easy_setopt(easy, CURLOPT_PROXYTYPE, curl_proxy_type == CURLPROXY_SOCKS5 ?
    					CURLPROXY_SOCKS5_HOSTNAME : curl_proxy_type);
				}
			}
		}
	}

	// Configure security settings
	configure_security(easy);

	// Add to multi handle
	CURLMcode rc = curl_multi_add_handle(m_multi, easy);
	if (rc != CURLM_OK) {
		curl_easy_cleanup(easy);
		handler(errors::http_error, {}); // Use generic HTTP error
		return nullptr;
	}

	m_requests[easy] = std::move(req);

	// Kickstart the transfer
	socket_action(CURL_SOCKET_TIMEOUT, 0);

	return easy;
}

void curl_tracker_manager::cancel_request(CURL* easy) {
	auto it = m_requests.find(easy);
	if (it != m_requests.end()) {
		curl_multi_remove_handle(m_multi, easy);
		curl_easy_cleanup(easy);
		m_requests.erase(it);
	}
}

void curl_tracker_manager::socket_action(curl_socket_t sock, int ev_bitmask) {
	// Don't process if shutting down
	if (m_shutting_down) return;

	int still_running = 0;
	CURLMcode rc = curl_multi_socket_action(m_multi, sock, ev_bitmask, &still_running);

	if (rc != CURLM_OK) {
		// Handle error - for now just return
		return;
	}

	// Check for completed transfers
	check_multi_info();

	// Update running count
	m_still_running = still_running;

	// Socket re-registration is now handled in the monitor_socket callbacks
	// curl will call socket_callback if it needs different events monitored
}

void curl_tracker_manager::timer_expired(error_code const& ec) {
	if (!ec) {
		socket_action(CURL_SOCKET_TIMEOUT, 0);
	}
}

void curl_tracker_manager::check_multi_info() {
	CURLMsg* msg;
	int msgs_left;

	// Collect completed requests to avoid iterator invalidation issues
	struct completed_request {
		CURL* easy;
		error_code ec;
		std::vector<char> response;
		std::function<void(error_code const&, std::vector<char> const&)> handler;
	};
	std::vector<completed_request> completed;

	while ((msg = curl_multi_info_read(m_multi, &msgs_left))) {
		if (msg->msg != CURLMSG_DONE) continue;

		CURL* easy = msg->easy_handle;
		auto it = m_requests.find(easy);
		if (it != m_requests.end()) {
			error_code ec;
			if (msg->data.result != CURLE_OK) {
				// Map curl error to libtorrent error
				ec = curl_error_to_libtorrent(msg->data.result);
			}

			// Save the request info before erasing
			completed_request req;
			req.easy = easy;
			req.ec = ec;
			req.response = std::move(it->second->response);
			req.handler = std::move(it->second->handler);
			completed.push_back(std::move(req));

			// Clean up from curl
			curl_multi_remove_handle(m_multi, easy);
			curl_easy_cleanup(easy);

			// Remove from our map
			m_requests.erase(it);
		}
	}

	// Now call handlers after all modifications to m_requests are complete
	// This prevents reentrancy issues if handlers trigger more requests
	for (auto& req : completed) {
		if (req.handler) {
			req.handler(req.ec, req.response);
		}
	}
}

}} // namespace libtorrent::aux

#endif // TORRENT_USE_LIBCURL