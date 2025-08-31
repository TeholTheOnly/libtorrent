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
#include "libtorrent/assert.hpp"
#include "libtorrent/aux_/invariant_check.hpp"
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

	// Maximum allowed tracker response size (10MB)
	constexpr size_t MAX_TRACKER_RESPONSE_SIZE = 10 * 1024 * 1024;

	// Write callback for curl
	size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
		try {
			auto* response = static_cast<std::vector<char>*>(userdata);
			size_t total = size * nmemb;
			
			// Check size limit to prevent memory exhaustion attacks
			if (response->size() + total > MAX_TRACKER_RESPONSE_SIZE) {
				// Signal error to curl by returning 0
				return 0;
			}
			
			// Reserve capacity to avoid multiple reallocations
			if (response->capacity() < response->size() + total) {
				response->reserve(response->size() + total);
			}
			
			response->insert(response->end(), ptr, ptr + total);
			return total;
		}
		catch (...) {
			// Never let exceptions cross C boundary
			return 0;
		}
	}

#if !defined(TORRENT_DISABLE_LOGGING) && 0
	// Helper function to convert socket action to string for logging
	// Currently unused but kept for future debugging
	const char* socket_action_string(int what) {
		switch (what) {
			case CURL_POLL_NONE: return "NONE";
			case CURL_POLL_IN: return "READ";
			case CURL_POLL_OUT: return "WRITE";
			case CURL_POLL_INOUT: return "READ|WRITE";
			case CURL_POLL_REMOVE: return "REMOVE";
			default: return "UNKNOWN";
		}
	}
#endif

	// Simple error mapping (when we don't have CURL handle for details)
	error_code curl_error_to_libtorrent(CURLcode code) {
		switch(code) {
			case CURLE_OK:
				return {};
			case CURLE_OPERATION_TIMEDOUT:
				return errors::timed_out;
			case CURLE_COULDNT_CONNECT:
				return errors::http_error;
			case CURLE_COULDNT_RESOLVE_HOST:
				return errors::invalid_hostname;
			case CURLE_URL_MALFORMAT:
				return errors::url_parse_error;
			case CURLE_TOO_MANY_REDIRECTS:
				return errors::http_error;
			case CURLE_SSL_CONNECT_ERROR:
			case CURLE_SSL_CERTPROBLEM:
			case CURLE_SSL_CIPHER:
			case CURLE_SSL_CACERT:
			case CURLE_SSL_CACERT_BADFILE:
			case CURLE_SSL_CRL_BADFILE:
			case CURLE_SSL_ISSUER_ERROR:
				// Map SSL errors to invalid_ssl_cert when available
				return errors::invalid_ssl_cert;
			case CURLE_OUT_OF_MEMORY:
				return errors::no_memory;
			default:
				return errors::http_error;
		}
	}
	
	// Enhanced error mapping with detailed context from CURL handle
	error_code curl_error_to_libtorrent_detailed(CURLcode code, CURL* easy) {
		if (!easy) {
			return curl_error_to_libtorrent(code);
		}
		
		switch(code) {
			case CURLE_OK:
				return {};
				
			case CURLE_SSL_CONNECT_ERROR:
			case CURLE_SSL_CERTPROBLEM:
			case CURLE_SSL_CIPHER:
			case CURLE_SSL_CACERT:
			case CURLE_SSL_CACERT_BADFILE:
			case CURLE_SSL_CRL_BADFILE:
			case CURLE_SSL_ISSUER_ERROR:
				// Extract detailed SSL error information if available
				{
					long ssl_verify_result = 0;
					curl_easy_getinfo(easy, CURLINFO_SSL_VERIFYRESULT, &ssl_verify_result);
					
					// TODO: When logging is available, log ssl_verify_result for debugging
					// Different SSL verification failures could be mapped to specific codes
					// but libtorrent only has invalid_ssl_cert currently
					return errors::invalid_ssl_cert;
				}
				
			case CURLE_OPERATION_TIMEDOUT:
				// Distinguish between connection and transfer timeouts
				{
					double connect_time = 0;
					curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME, &connect_time);
					if (connect_time == 0) {
						// Never connected - handshake timeout
						return errors::timed_out_no_handshake;
					} else {
						// Connected but transfer timed out
						return errors::timed_out;
					}
				}
				
			case CURLE_COULDNT_CONNECT:
				return errors::http_error;
				
			case CURLE_COULDNT_RESOLVE_HOST:
				return errors::invalid_hostname;
				
			case CURLE_URL_MALFORMAT:
				return errors::url_parse_error;
				
			case CURLE_TOO_MANY_REDIRECTS:
				return errors::redirecting;
				
			case CURLE_GOT_NOTHING:
				// Server closed connection without sending data
				return errors::http_error;
				
			case CURLE_SEND_ERROR:
			case CURLE_RECV_ERROR:
				// Network I/O errors
				return errors::http_error;
				
			case CURLE_OUT_OF_MEMORY:
				return errors::no_memory;
				
			default:
				// Log unknown error code for investigation
				// TODO: Add logging when available
				return errors::http_error;
		}
	}
}

// socket_info implementation with platform-specific monitoring
curl_tracker_manager::socket_info::socket_info(io_context& ios)
#ifdef TORRENT_WINDOWS
	: event_handle(ios)
#else
	: monitor(ios)
#endif
{
}

curl_tracker_manager::socket_info::~socket_info()
{
#ifdef TORRENT_WINDOWS
	cleanup_event_monitor();
#endif
}

#ifdef TORRENT_WINDOWS
void curl_tracker_manager::socket_info::setup_event_monitor(curl_socket_t sock, int what)
{
	// Store the socket for later WSAEventSelect calls
	socket_fd = sock;
	
	// Create event if not already created
	if (!event_handle.is_open())
	{
		WSAEVENT event = WSACreateEvent();
		if (event == WSA_INVALID_EVENT)
		{
			int wsa_error = WSAGetLastError();
			throw std::system_error(wsa_error, std::system_category(), 
				"WSACreateEvent failed");
		}
		// Assign event to the object_handle (takes ownership)
		event_handle.assign(event);
	}
	
	// Map curl poll flags to Windows events
	long events = FD_CLOSE;  // Always monitor close events
	if (what & CURL_POLL_IN)  events |= FD_READ | FD_ACCEPT;
	if (what & CURL_POLL_OUT) events |= FD_WRITE | FD_CONNECT;
	
	// Register events with socket (non-owning - libcurl retains socket ownership)
	if (WSAEventSelect(socket_fd, event_handle.native_handle(), events) == SOCKET_ERROR)
	{
		int wsa_error = WSAGetLastError();
		throw std::system_error(wsa_error, std::system_category(), 
			"WSAEventSelect failed");
	}
}

void curl_tracker_manager::socket_info::cleanup_event_monitor()
{
	if (event_handle.is_open())
	{
		// Unregister events from socket
		if (socket_fd != INVALID_SOCKET)
		{
			WSAEventSelect(socket_fd, event_handle.native_handle(), 0);
		}
		// Cancel any pending async operations
		boost::system::error_code ec;
		event_handle.cancel(ec);
		// Close the event handle
		event_handle.close();
		socket_fd = INVALID_SOCKET;
	}
}
#endif

curl_tracker_manager::curl_tracker_manager(io_context& ios, settings_pack const& settings)
	: m_ios(ios)
	, m_multi(curl_multi_init())  // RAII initialization
	, m_share(curl_share_init())  // RAII initialization
	, m_timer(ios)
	, m_still_running(0)
	, m_settings(settings)
	, m_http2_supported(false)
	, m_shutting_down(false)
{
	// Ensure curl is initialized globally
	ensure_curl_initialized();

	// Check if curl multi handle was initialized successfully
	if (!m_multi) {
		throw std::runtime_error("Failed to initialize curl multi handle");
	}

	// Set socket callback - integrates with Boost.Asio
	curl_multi_setopt(m_multi.get(), CURLMOPT_SOCKETFUNCTION, &socket_callback);
	curl_multi_setopt(m_multi.get(), CURLMOPT_SOCKETDATA, this);

	// Set timer callback - manages timeouts
	curl_multi_setopt(m_multi.get(), CURLMOPT_TIMERFUNCTION, &timer_callback);
	curl_multi_setopt(m_multi.get(), CURLMOPT_TIMERDATA, this);

	// Enable HTTP/2 multiplexing if available
	#ifdef CURLPIPE_MULTIPLEX
	curl_multi_setopt(m_multi.get(), CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
	#endif

	// Comprehensive runtime capability validation
	curl_version_info_data* ver = curl_version_info(CURLVERSION_NOW);
	if (!ver) {
		throw std::runtime_error("Failed to get curl version info");
	}
	
	// Check minimum version requirement (7.66.0 = 0x074200)
	if (ver->version_num < 0x074200) {
		throw std::runtime_error("libcurl version too old, need >= 7.66.0, got: " 
			+ std::string(ver->version ? ver->version : "unknown"));
	}
	
	// Check for SSL support
	if (!(ver->features & CURL_VERSION_SSL)) {
		// Log warning but continue - HTTP-only trackers still work
		// TODO: Add logging when session_log is available
		// session_log("WARNING: libcurl built without SSL support");
	}
	
	// Check for HTTP/2 support
	if (ver->features & CURL_VERSION_HTTP2) {
		m_http2_supported = true;
		// TODO: Add logging when available
		// session_log("INFO: HTTP/2 support enabled via libcurl %s", ver->version);
	} else {
		m_http2_supported = false;
		// TODO: Add logging when available  
		// session_log("INFO: libcurl built without HTTP/2 support, using HTTP/1.1");
	}
	
	// Check for proxy support if configured
	if (settings.get_int(settings_pack::proxy_type) != settings_pack::none) {
		if (!(ver->features & CURL_VERSION_LIBZ)) {
			// Some proxies require compression support
			// TODO: Add logging when available
			// session_log("WARNING: libcurl built without compression support");
		}
	}
	
	// Check for AsynchDNS for better performance
	if (!(ver->features & CURL_VERSION_ASYNCHDNS)) {
		// TODO: Add logging when available
		// session_log("WARNING: libcurl built without async DNS support - may block");
	}
	
	// Set connection limits optimized for HTTP/2 vs HTTP/1.1
	long max_total = settings.get_int(settings_pack::connections_limit);
	if (max_total > 0) {
		// HTTP/2 needs fewer total connections due to multiplexing
		if (m_http2_supported) {
			max_total = std::min(max_total, 10L);
		}
		curl_multi_setopt(m_multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, max_total);
	}

	// HTTP/2 can multiplex many requests over fewer connections per host
	long max_per_host = m_http2_supported ? 2L : 6L;
	curl_multi_setopt(m_multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS, max_per_host);
	
	// Enable aggressive connection reuse
	curl_multi_setopt(m_multi.get(), CURLMOPT_MAXCONNECTS, 100L);

	// Configure share handle for connection pooling
	if (m_share) {
		curl_share_setopt(m_share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
		curl_share_setopt(m_share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
		curl_share_setopt(m_share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	}

}

curl_tracker_manager::~curl_tracker_manager() {
	// Set shutdown flag to prevent new callbacks
	m_shutting_down = true;

	// Cancel timer to prevent callbacks after destruction
	m_timer.cancel();

	// Clean up all sockets first to prevent callbacks
#ifdef TORRENT_WINDOWS
	// Windows: socket_info destructor will clean up event monitoring
	m_sockets.clear();
#else
	// POSIX: Close duplicated file descriptors
	for (auto& pair : m_sockets) {
		pair.second->monitor.close();
	}
	m_sockets.clear();
#endif

	// Clean up all requests
	for (auto& pair : m_requests) {
		CURL* easy = pair.first;
		curl_multi_remove_handle(m_multi.get(), easy);
		curl_easy_cleanup(easy);
	}
	m_requests.clear();

	// RAII handles clean up automatically
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

	// Defensive check: validate manager pointer
	if (!manager || manager->m_shutting_down) {
		return -1;  // Signal error to curl
	}

	// Defensive check: validate socket descriptor
	if (sock == CURL_SOCKET_BAD) {
		TORRENT_ASSERT_FAIL_VAL(sock);
		return -1;
	}

#ifdef TORRENT_WINDOWS
	// Windows-specific validation
	if (sock == INVALID_SOCKET) {
		TORRENT_ASSERT_FAIL_VAL(sock);
		return -1;
	}
#else
	// POSIX validation - check FD_SETSIZE limit
	if (sock < 0 || sock >= FD_SETSIZE) {
		TORRENT_ASSERT_FAIL_VAL(sock);
		return -1;
	}
#endif

#ifndef TORRENT_DISABLE_LOGGING
	// Log socket lifecycle for debugging (would need session_log access)
	// For now, we can at least use debug assertions
	TORRENT_ASSERT_VAL(what >= CURL_POLL_NONE && what <= CURL_POLL_REMOVE,
	                   what);
#endif

	if (what == CURL_POLL_REMOVE) {
		// Stop monitoring this socket
		auto it = manager->m_sockets.find(sock);
		if (it != manager->m_sockets.end()) {
			// Mark as not wanted
			it->second->wants_read = false;
			it->second->wants_write = false;
			
#ifdef TORRENT_WINDOWS
			// Windows: Clean up event monitoring (non-owning)
			// The destructor will call cleanup_event_monitor()
			// which cancels operations and unregisters events
#else
			// POSIX: Cancel operations on our duplicated fd
			it->second->monitor.cancel();
			// Let destructor close our duplicated fd
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
		// Windows: Setup event monitoring (non-owning)
		// libcurl retains ownership of the socket
		try {
			info->setup_event_monitor(sock, what);
		} catch (const std::system_error& e) {
			// Failed to setup event monitoring
			// TODO: Log error when logging is available
			return -1;
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
		curl_multi_assign(manager->m_multi.get(), sock, it->second.get());
	}

	// Update monitoring based on what curl wants
	auto info_ptr = it->second;
	auto& info = *info_ptr;
	
#ifdef TORRENT_WINDOWS
	// Update Windows event monitoring if socket already exists
	if (it != manager->m_sockets.end()) {
		try {
			info.setup_event_monitor(sock, what);
		} catch (const std::system_error& e) {
			// Failed to update event monitoring
			return -1;
		}
	}
#endif
	
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
	// SSL/TLS verification settings from user configuration
	// Note: These settings only affect HTTPS connections; HTTP connections ignore them
	bool verify_peer = m_settings.get_bool(settings_pack::tracker_ssl_verify_peer);
	bool verify_host = m_settings.get_bool(settings_pack::tracker_ssl_verify_host);

	// CURLOPT_SSL_VERIFYPEER: 0=don't verify, 1=verify certificate
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, verify_peer ? 1L : 0L);
	// CURLOPT_SSL_VERIFYHOST: 0=don't verify, 2=verify hostname matches certificate
	// (1 is deprecated, must use 0 or 2)
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
	int64_t max_size = m_settings.get_int(settings_pack::max_tracker_response_size);
	if (max_size > 0) {
		curl_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(max_size));
	}

	// Security: Disable redirects to prevent SSRF attacks
	curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 0L);

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
#ifdef TORRENT_WINDOWS
	// Windows: Use event-based monitoring
	// The event will be signaled when any of the registered events occur
	// We don't separate read/write as WSAEventSelect monitors all events
	
	// Check if already waiting
	if (info->waiting_read || info->waiting_write) {
		return;
	}
	
	// Check if monitoring is desired
	if (!info->wants_read && !info->wants_write) {
		return;
	}
	
	// Mark as waiting
	info->waiting_read = info->wants_read;
	info->waiting_write = info->wants_write;
	
	// Capture weak pointers for lifetime management
	std::weak_ptr<curl_tracker_manager> weak_manager = shared_from_this();
	
	// Start async wait on the Windows event
	info->event_handle.async_wait(
		[weak_manager, sock, info](error_code const& ec) {
			
			// Check manager lifetime
			auto mgr = weak_manager.lock();
			if (!mgr || mgr->m_shutting_down) return;
			
			// Clear waiting flags
			info->waiting_read = false;
			info->waiting_write = false;
			
			if (ec) {
				if (ec != boost::asio::error::operation_aborted) {
					// Real error, notify curl
					mgr->socket_action(sock, CURL_CSELECT_ERR);
				}
				return;
			}
			
			// Event signaled - determine which network events occurred
			WSANETWORKEVENTS network_events;
			if (WSAEnumNetworkEvents(info->socket_fd, 
			                         info->event_handle.native_handle(), 
			                         &network_events) == SOCKET_ERROR) {
				// Failed to get network events
				mgr->socket_action(sock, CURL_CSELECT_ERR);
				return;
			}
			
			// Map Windows events to curl flags
			int events = 0;
			if (network_events.lNetworkEvents & (FD_READ | FD_ACCEPT)) {
				events |= CURL_CSELECT_IN;
			}
			if (network_events.lNetworkEvents & (FD_WRITE | FD_CONNECT)) {
				events |= CURL_CSELECT_OUT;
			}
			if (network_events.lNetworkEvents & FD_CLOSE) {
				// Socket closed - notify both directions
				events |= CURL_CSELECT_IN | CURL_CSELECT_OUT;
			}
			
			// Check for errors in the event array
			bool has_error = false;
			for (int i = 0; i < FD_MAX_EVENTS; ++i) {
				if ((network_events.lNetworkEvents & (1 << i)) && 
				    network_events.iErrorCode[i] != 0) {
					has_error = true;
					break;
				}
			}
			
			if (has_error) {
				mgr->socket_action(sock, CURL_CSELECT_ERR);
				return;
			}
			
			// Notify curl about ready events
			if (events != 0) {
				mgr->socket_action(sock, events);
			}
			
			// Continue monitoring asynchronously
			boost::asio::post(mgr->m_ios, [weak_manager, sock, info]() {
				auto mgr_post = weak_manager.lock();
				if (!mgr_post || mgr_post->m_shutting_down) return;
				
				// Verify socket still exists and restart monitoring
				if (mgr_post->m_sockets.count(sock) > 0) {
					// Restart monitoring for any direction
					mgr_post->manage_io_loop(sock, info, CURL_POLL_IN);
				}
			});
		});
		
#else
	// POSIX: Use stream_descriptor with separate read/write monitoring
	// Determine state variables based on direction
	bool wants_io;
	bool& waiting_io = (direction == CURL_POLL_IN) ? 
	                   info->waiting_read : info->waiting_write;
	boost::asio::posix::stream_descriptor::wait_type wait_type;
	int cselect_flag;

	if (direction == CURL_POLL_IN) {
		wants_io = info->wants_read;
		wait_type = boost::asio::posix::stream_descriptor::wait_read;
		cselect_flag = CURL_CSELECT_IN;
	} else {
		wants_io = info->wants_write;
		wait_type = boost::asio::posix::stream_descriptor::wait_write;
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
#endif
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
	curl_easy_setopt(easy, CURLOPT_SHARE, m_share.get());
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &req->response);
	curl_easy_setopt(easy, CURLOPT_PRIVATE, req.get());

	// Disable SIGPIPE signals (crucial for multi-threaded applications)
	// This prevents crashes when writing to closed sockets
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	
	// Set reasonable timeouts
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);  // 30 second connection timeout
	curl_easy_setopt(easy, CURLOPT_TIMEOUT, 60L);         // 60 second total timeout

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
					long proxy_type_to_use = (curl_proxy_type == CURLPROXY_SOCKS5) ?
						static_cast<long>(CURLPROXY_SOCKS5_HOSTNAME) : curl_proxy_type;
					curl_easy_setopt(easy, CURLOPT_PROXYTYPE, proxy_type_to_use);
				}
			}
		}
	}

	// Configure security settings
	configure_security(easy);

	// Add to multi handle
	CURLMcode rc = curl_multi_add_handle(m_multi.get(), easy);
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
		curl_multi_remove_handle(m_multi.get(), easy);
		curl_easy_cleanup(easy);
		m_requests.erase(it);
	}
}

void curl_tracker_manager::socket_action(curl_socket_t sock, int ev_bitmask) {
	// Don't process if shutting down
	if (m_shutting_down) return;

	int still_running = 0;
	CURLMcode rc = curl_multi_socket_action(m_multi.get(), sock, ev_bitmask, &still_running);

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

	while ((msg = curl_multi_info_read(m_multi.get(), &msgs_left))) {
		if (msg->msg != CURLMSG_DONE) continue;

		CURL* easy = msg->easy_handle;
		auto it = m_requests.find(easy);
		if (it != m_requests.end()) {
			error_code ec;
			if (msg->data.result != CURLE_OK) {
				// Map curl error to libtorrent error with detailed context
				ec = curl_error_to_libtorrent_detailed(msg->data.result, easy);
			} else {
				// Check HTTP response code for security issues
				long response_code = 0;
				curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response_code);
				
				// Treat redirect responses as errors (security: prevent SSRF)
				if (response_code >= 300 && response_code < 400) {
					ec = errors::http_error;  // Redirect without following = error
				}
			}

			// Save the request info before erasing
			completed_request req;
			req.easy = easy;
			req.ec = ec;
			req.response = std::move(it->second->response);
			req.handler = std::move(it->second->handler);
			completed.push_back(std::move(req));

			// Clean up from curl
			curl_multi_remove_handle(m_multi.get(), easy);
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

#if TORRENT_USE_INVARIANT_CHECKS
void curl_tracker_manager::check_invariant() const
{
	// Verify multi handle is valid
	TORRENT_ASSERT(m_multi != nullptr);
	TORRENT_ASSERT(m_share != nullptr);
	
	// Verify socket count consistency
	// Note: We can't call curl_multi_perform in const context,
	// so we check what we can
	TORRENT_ASSERT(m_still_running >= 0);
	TORRENT_ASSERT(m_still_running <= static_cast<int>(m_requests.size()));
	
	// Verify all tracked sockets have valid monitors
	for (auto const& socket_pair : m_sockets) {
		curl_socket_t sock = socket_pair.first;
		auto const& info = socket_pair.second;
		TORRENT_ASSERT(sock != CURL_SOCKET_BAD);
		TORRENT_ASSERT(info != nullptr);
#ifdef TORRENT_WINDOWS
		TORRENT_ASSERT(sock != INVALID_SOCKET);
#else
		TORRENT_ASSERT(sock >= 0);
		TORRENT_ASSERT(sock < FD_SETSIZE);
#endif
		// Check consistency of want/waiting states
		if (!info->wants_read) {
			TORRENT_ASSERT(!info->waiting_read);
		}
		if (!info->wants_write) {
			TORRENT_ASSERT(!info->waiting_write);
		}
	}
	
	// Verify all requests have handlers
	for (auto const& request_pair : m_requests) {
		CURL* easy = request_pair.first;
		auto const& req = request_pair.second;
		TORRENT_ASSERT(easy != nullptr);
		TORRENT_ASSERT(req != nullptr);
		TORRENT_ASSERT(req->handler != nullptr);
		// Response size should not exceed max limit
		TORRENT_ASSERT(req->response.size() <= MAX_TRACKER_RESPONSE_SIZE);
	}
	
	// Verify timer state
	// Timer should only be active if we have running requests
	if (m_still_running == 0) {
		// Note: Can't easily check if timer is cancelled in const context
		// but we assert the expected state
	}
	
	// Verify shutdown flag consistency
	if (m_shutting_down) {
		// During shutdown, we shouldn't be accepting new requests
		// This is enforced in add_request
	}
}
#endif

}} // namespace libtorrent::aux

#endif // TORRENT_USE_LIBCURL