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

#ifndef TORRENT_CURL_TRACKER_MANAGER_HPP
#define TORRENT_CURL_TRACKER_MANAGER_HPP

#include "libtorrent/config.hpp"

#ifdef TORRENT_USE_LIBCURL

#include "libtorrent/io_context.hpp"
#include "libtorrent/deadline_timer.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/settings_pack.hpp"
#include "libtorrent/error_code.hpp"
#include <curl/curl.h>
#include <memory>
#include <unordered_map>
#include <map>
#include <vector>
#include <functional>

#ifndef TORRENT_WINDOWS
// For monitoring curl's file descriptors without taking ownership
#include <boost/asio/posix/stream_descriptor.hpp>
#endif

namespace libtorrent { namespace aux {

// Platform-specific socket monitor type
// Windows must use tcp::socket (with careful ownership handling)
// POSIX can use stream_descriptor (non-owning fd monitoring)
#ifdef TORRENT_WINDOWS
using platform_socket_monitor = boost::asio::ip::tcp::socket;
#else
using platform_socket_monitor = boost::asio::posix::stream_descriptor;
#endif

// Direct integration with libcurl multi interface
// Manages socket callbacks and timer integration with Boost.Asio
class TORRENT_EXPORT curl_tracker_manager 
	: public std::enable_shared_from_this<curl_tracker_manager> 
{
public:
	curl_tracker_manager(io_context& ios, settings_pack const& settings);
	~curl_tracker_manager();
	
	// Add a new HTTP request
	// Returns a handle that can be used to cancel the request
	CURL* add_request(
		std::string const& url,
		std::function<void(error_code const&, std::vector<char> const&)> handler);
	
	// Cancel a specific request
	void cancel_request(CURL* easy);
	
	// Check if HTTP/2 is supported by libcurl
	bool supports_http2() const;
	
private:
	// Socket callback from curl multi
	static int socket_callback(CURL* easy, curl_socket_t sock, int what,
	                          void* userp, void* socketp);
	
	// Timer callback from curl multi
	static int timer_callback(CURLM* multi, long timeout_ms, void* userp);
	
	// Process curl multi events
	void socket_action(curl_socket_t sock, int ev_bitmask);
	void timer_expired(error_code const& ec);
	void check_multi_info();
	
	// Socket management
	struct socket_info {
		explicit socket_info(io_context& ios) : monitor(ios) {}
		platform_socket_monitor monitor;
		
		// Separate intent (what curl wants) from action (what Asio is doing)
		bool wants_read = false;   // Intent: curl requested CURL_POLL_IN
		bool waiting_read = false;  // Action: async_wait(wait_read) is active
		
		bool wants_write = false;  // Intent: curl requested CURL_POLL_OUT
		bool waiting_write = false; // Action: async_wait(wait_write) is active
	};
	
	// Request tracking
	struct request_info {
		CURL* easy;
		std::vector<char> response;
		std::function<void(error_code const&, std::vector<char> const&)> handler;
	};
	
	// Configure security settings for a curl easy handle
	void configure_security(CURL* easy);
	
	// Helper to monitor socket with re-registration
	void monitor_socket(curl_socket_t sock, socket_info& info, int what);
	
	// Robust asynchronous I/O loop with proper continuation
	void manage_io_loop(curl_socket_t sock, std::shared_ptr<socket_info> info, int direction);
	
private:
	io_context& m_ios;
	CURLM* m_multi;         // curl multi handle
	CURLSH* m_share;        // curl share handle for connection pooling
	deadline_timer m_timer; // timeout timer
	
	std::map<curl_socket_t, std::shared_ptr<socket_info>> m_sockets;
	std::unordered_map<CURL*, std::unique_ptr<request_info>> m_requests;
	
	int m_still_running;
	settings_pack m_settings;
	bool m_http2_supported;
	bool m_shutting_down;   // Flag to prevent callbacks during destruction
};

}} // namespace libtorrent::aux

#endif // TORRENT_USE_LIBCURL

#endif // TORRENT_CURL_TRACKER_MANAGER_HPP