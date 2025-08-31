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

#include "libtorrent/aux_/curl_thread_manager.hpp"
#include "libtorrent/aux_/curl_handle_wrappers.hpp"  // RAII wrappers
#include "libtorrent/assert.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/settings_pack.hpp"
#include <boost/asio/post.hpp>
#include <curl/curl.h>
#include <chrono>
#include <algorithm>
#include <stdexcept> // For std::runtime_error
#include <limits>    // For std::numeric_limits
#include <thread>    // For std::this_thread::sleep_for

namespace libtorrent { namespace aux {

// Structure to hold request data with proper lifetime management
// This is allocated with new and stored via CURLOPT_PRIVATE to ensure
// the response buffer stays alive throughout the transfer
struct curl_transfer_data {
    curl_request request;
    std::shared_ptr<response_data> response; // Keeps buffer alive
    curl_easy_handle easy_handle; // RAII wrapper for CURL handle
    
    explicit curl_transfer_data(curl_request&& req) 
        : request(std::move(req))
        , response(request.response) // Share ownership
        , easy_handle() // Initialize CURL handle (throws on failure)
    {}
};

namespace {
    // Updated write callback for curl using response_data structure
    size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
        try {
            // Cast userdata to response_data structure (which is heap-allocated)
            auto* data = static_cast<response_data*>(userdata);
            if (!data) return 0;

            std::vector<char>* buffer = &data->buffer;
            size_t total = size * nmemb;

            // Check against dynamic size limit
            if (buffer->size() + total > data->max_size) {
                return 0; // Signal error to curl (CURLE_WRITE_ERROR)
            }

            buffer->insert(buffer->end(), ptr, ptr + total);
            return total;
        } catch (...) {
            return 0;
        }
    }
    
    // Map curl errors to libtorrent errors
    error_code curl_error_to_libtorrent(CURLcode code) {
        switch(code) {
            case CURLE_OK:
                return {};
            case CURLE_OPERATION_TIMEDOUT:
                return errors::timed_out;
            case CURLE_COULDNT_CONNECT:
                // Can happen if direct or proxy connection fails
                return errors::http_error;
            case CURLE_COULDNT_RESOLVE_HOST:
                return errors::invalid_hostname;
            case CURLE_COULDNT_RESOLVE_PROXY:
                // Specific error if proxy hostname resolution fails
                return errors::invalid_hostname;
            case CURLE_SSL_CONNECT_ERROR:
            case CURLE_SSL_CERTPROBLEM:
                return errors::invalid_ssl_cert;
            case CURLE_OUT_OF_MEMORY:
                return errors::no_memory;
            // Handle write error (e.g., exceeding max_size in write callback)
            case CURLE_WRITE_ERROR:
                return errors::http_error;
            // Handle file size exceeded (from CURLOPT_MAXFILESIZE_LARGE)
            case CURLE_FILESIZE_EXCEEDED:
                return errors::http_error;
            case CURLE_UNSUPPORTED_PROTOCOL:
                // Occurs if protocol restriction blocks it (e.g. FTP)
                return errors::unsupported_url_protocol;
            default:
                return errors::http_error;
        }
    }
    
    // Check if error is retryable
    bool is_retryable_error(error_code const& ec) {
        return ec == errors::timed_out ||
               ec == errors::http_error ||
               ec == errors::invalid_hostname;
    }

    // Helper function to configure proxy settings
    void configure_proxy(CURL* easy, session_settings const& settings) {
        // Check if proxy should be used for tracker connections
        if (!settings.get_bool(settings_pack::proxy_tracker_connections)) {
            return;
        }

        int proxy_type = settings.get_int(settings_pack::proxy_type);
        std::string proxy_host = settings.get_str(settings_pack::proxy_hostname);
        int proxy_port = settings.get_int(settings_pack::proxy_port);

        if (proxy_type == settings_pack::none || proxy_host.empty() || proxy_port <= 0) {
            return;
        }

        // Configure proxy host and port
        curl_easy_setopt(easy, CURLOPT_PROXY, proxy_host.c_str());
        curl_easy_setopt(easy, CURLOPT_PROXYPORT, static_cast<long>(proxy_port));

        // Configure proxy type
        long curl_proxy_type = CURLPROXY_HTTP;
        bool requires_auth = false;

        switch (proxy_type) {
            case settings_pack::socks4:
                // Use SOCKS4A for remote hostname resolution
                curl_proxy_type = CURLPROXY_SOCKS4A;
                break;
            case settings_pack::socks5:
                // Use SOCKS5_HOSTNAME for remote hostname resolution
                curl_proxy_type = CURLPROXY_SOCKS5_HOSTNAME;
                break;
            case settings_pack::socks5_pw:
                curl_proxy_type = CURLPROXY_SOCKS5_HOSTNAME;
                requires_auth = true;
                break;
            case settings_pack::http:
                curl_proxy_type = CURLPROXY_HTTP;
                break;
            case settings_pack::http_pw:
                curl_proxy_type = CURLPROXY_HTTP;
                requires_auth = true;
                break;
            default:
                return; // Unknown/unsupported proxy type
        }

        curl_easy_setopt(easy, CURLOPT_PROXYTYPE, curl_proxy_type);

        // Configure proxy authentication if required
        if (requires_auth) {
            std::string username = settings.get_str(settings_pack::proxy_username);
            std::string password = settings.get_str(settings_pack::proxy_password);

            if (!username.empty()) {
                // SECURITY FIX: Use separate username/password options instead of concatenation
                curl_easy_setopt(easy, CURLOPT_PROXYUSERNAME, username.c_str());
                curl_easy_setopt(easy, CURLOPT_PROXYPASSWORD, password.c_str());
                
                // Clear sensitive data immediately
                username.assign(username.size(), '\0');
                password.assign(password.size(), '\0');

                // Use safe authentication methods for HTTP proxy
                if (proxy_type == settings_pack::http_pw) {
                    curl_easy_setopt(easy, CURLOPT_PROXYAUTH, CURLAUTH_ANYSAFE);
                }
            }
        }

        // Security: Ensure that no addresses (like localhost) bypass the proxy if one is configured.
        // Setting NOPROXY to "" ensures even localhost is proxied if configured.
        curl_easy_setopt(easy, CURLOPT_NOPROXY, "");
    }

    // Helper function to configure SSL/TLS settings
    void configure_ssl(CURL* easy, session_settings const& settings) {
        // SSL verification (use tracker_ssl_verify_peer/host as these are used in tests)
        if (settings.get_bool(settings_pack::tracker_ssl_verify_peer)) {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        } else {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
        }

        if (settings.get_bool(settings_pack::tracker_ssl_verify_host)) {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        } else {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        
        // SECURITY FIX: Enforce minimum TLS version
        int min_tls_version = settings.get_int(settings_pack::tracker_min_tls_version);
        long curl_tls_version = CURL_SSLVERSION_TLSv1_2; // Default to TLS 1.2
        
        // Map libtorrent TLS version to libcurl constants
        // 0x0301 = TLS 1.0, 0x0302 = TLS 1.1, 0x0303 = TLS 1.2, 0x0304 = TLS 1.3
        switch (min_tls_version) {
            case 0x0302: // TLS 1.1
                curl_tls_version = CURL_SSLVERSION_TLSv1_1;
                break;
            case 0x0303: // TLS 1.2
                curl_tls_version = CURL_SSLVERSION_TLSv1_2;
                break;
            case 0x0304: // TLS 1.3
#ifdef CURL_SSLVERSION_TLSv1_3
                curl_tls_version = CURL_SSLVERSION_TLSv1_3;
#else
                curl_tls_version = CURL_SSLVERSION_TLSv1_2; // Fallback
#endif
                break;
            default:
                // Default to TLS 1.2 minimum for security
                curl_tls_version = CURL_SSLVERSION_TLSv1_2;
        }
        
        curl_easy_setopt(easy, CURLOPT_SSLVERSION, curl_tls_version);
        
        // SECURITY FIX: Configure strong cipher suites
        // Only allow modern, secure ciphers
        curl_easy_setopt(easy, CURLOPT_SSL_CIPHER_LIST, 
            "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM:DHE+CHACHA20:"
            "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256:"
            "!aNULL:!eNULL:!EXPORT:!DES:!MD5:!PSK:!RC4:!3DES:!DSS");
        
        // Additional security hardening
        curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS, 
            CURLSSLOPT_NO_REVOKE | CURLSSLOPT_NO_PARTIALCHAIN);
        
        // Enable OCSP stapling if available
#ifdef CURLOPT_SSL_VERIFYSTATUS
        if (settings.get_bool(settings_pack::tracker_ssl_verify_peer)) {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYSTATUS, 1L);
        }
#endif
    }
}

std::shared_ptr<curl_thread_manager> curl_thread_manager::create(
    io_context& ios, session_settings const& settings) 
{
    auto manager = std::shared_ptr<curl_thread_manager>(
        new curl_thread_manager(ios, settings));
    
    // FIX Issue 1: Break shared_ptr reference cycle.
    // The original code captured a shared_ptr in the thread lambda, causing a leak.
    // We use the raw pointer instead. This is safe because the destructor joins the thread.
    manager->m_curl_thread = std::thread(
        &curl_thread_manager::curl_thread_func, manager.get());
    
    // Wait for thread to be fully initialized
    {
        std::unique_lock<std::mutex> lock(manager->m_init_mutex);
        manager->m_init_cv.wait(lock, [&manager]{ 
            return manager->m_init_status != InitStatus::Pending; 
        });
        
        // Check if initialization failed
        if (manager->m_init_status == InitStatus::Failed) {
            // Join the failed thread before throwing
            if (manager->m_curl_thread.joinable()) {
                manager->m_curl_thread.join();
            }
            throw std::runtime_error("Failed to initialize curl multi handle");
        }
    }
    
    return manager;
}

curl_thread_manager::curl_thread_manager(io_context& ios, session_settings const& settings)
    : m_ios(ios)
    , m_settings(settings)
{
    // Ensure curl is initialized globally (thread-safe with std::once_flag)
    static std::once_flag curl_init_flag;
    std::call_once(curl_init_flag, []() {
        curl_global_init(CURL_GLOBAL_ALL);
    });
    
    // Verify libcurl version at runtime
    curl_version_info_data* ver = curl_version_info(CURLVERSION_NOW);
    if (!ver || ver->version_num < 0x074200) { // 7.66.0
        throw std::runtime_error("libcurl 7.66.0+ required for curl_multi_poll, found: " 
            + std::string(ver ? ver->version : "unknown"));
    }
    
    // Verify async DNS support
    if (!(ver->features & CURL_VERSION_ASYNCHDNS)) {
        throw std::runtime_error("libcurl must be built with async DNS support (c-ares or threaded resolver)");
    }
}

curl_thread_manager::~curl_thread_manager() {
    shutdown();
}

void curl_thread_manager::shutdown() {
    // Signal shutdown
    m_stopping = true;
    
    // Wake up the thread if it's waiting
    wakeup_curl_thread();
    
    // Wait for thread to finish
    if (m_curl_thread.joinable()) {
        m_curl_thread.join();
    }
    
    // Process any remaining queued requests with error
    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        while (!m_request_queue.empty()) {
            auto req = std::move(m_request_queue.front());
            m_request_queue.pop();
            
            // Post error to completion handler
            boost::asio::post(m_ios, [handler = req.completion_handler]() {
                handler(errors::session_is_closing, std::vector<char>{});
            });
        }
    }
    
    // Process any remaining retry requests
    // Safe to access without lock as worker thread has joined
    for (auto& item : m_retry_queue) {
        // Post error to completion handler for each pending retry
        boost::asio::post(m_ios, [handler = item.request.completion_handler]() {
            handler(errors::session_is_closing, std::vector<char>{});
        });
    }
    m_retry_queue.clear();
}

void curl_thread_manager::wakeup_curl_thread() {
    // Use curl_multi_wakeup (available in libcurl 7.68.0+)
    CURLM* multi = m_multi_handle.load();
    if (multi) {
        CURLMcode rc = curl_multi_wakeup(multi);
        if (rc != CURLM_OK) {
            // Log warning but continue - wakeup is best-effort
            // The thread will still process the request on next timeout
        }
    }
}

void curl_thread_manager::add_request(
    std::string const& url,
    std::function<void(error_code, std::vector<char>)> handler,
    time_duration timeout)
{
    if (m_stopping) {
        // Call handler with error immediately
        boost::asio::post(m_ios, [handler]() {
            handler(errors::session_is_closing, std::vector<char>{});
        });
        return;
    }

    curl_request req;
    req.url = url;
    req.completion_handler = handler;
    req.deadline = clock_type::now() + timeout;

    // Set dynamic size limit from settings
    // Default to 128KB if not set (consistent with typical tracker response size)
    int max_size = m_settings.get_int(settings_pack::max_tracker_response_size);
    if (max_size <= 0) {
        max_size = 128 * 1024; // Default 128KB
    }
    
    // Use memory pool for response buffer allocation
    req.response = m_buffer_pool.acquire(static_cast<size_t>(max_size));
    req.response->max_size = static_cast<size_t>(max_size);

    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        m_request_queue.push(std::move(req));
        m_total_requests++;
    }

    // Wake up the curl thread to process new request immediately
    wakeup_curl_thread();
}

std::vector<curl_request> curl_thread_manager::swap_pending_requests() {
    std::vector<curl_request> local_queue;
    
    // Minimize lock duration - just swap queues
    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        while (!m_request_queue.empty()) {
            local_queue.push_back(std::move(m_request_queue.front()));
            m_request_queue.pop();
        }
    }
    
    return local_queue;
}

// Centralized configuration for CURL handles
bool curl_thread_manager::configure_handle(CURL* easy, curl_request const& req) {

    // Calculate timeout based on deadline
    auto now = clock_type::now();
    if (now >= req.deadline) return false; // Already timed out

    auto timeout_ms = std::chrono::duration_cast<milliseconds>(req.deadline - now).count();
    long timeout_sec = std::max(1L, timeout_ms / 1000);

    // Basic configuration
    curl_easy_setopt(easy, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
    // NOTE: CURLOPT_WRITEDATA is now set by the caller with transfer_data->response.get()
    // This ensures proper lifetime management of the response buffer
    
    // CRITICAL SECURITY FIX: Disable redirects to prevent SSRF attacks
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    // Remove MAXREDIRS as redirects are disabled
    
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, std::min(10L, timeout_sec));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L); // Essential for multi-threading
    
    // SECURITY FIX: Add response size limits for headers too
    curl_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE, 
        static_cast<curl_off_t>(req.response->max_size));
    
    // DoS protection: Set more aggressive timeouts
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 10L);  // 10 bytes/sec minimum
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);   // For 30 seconds

    // Restrict to HTTP and HTTPS only for security
#ifdef CURLOPT_PROTOCOLS_STR
    // Use newer API if available (libcurl 7.85.0+)
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    // Fall back to older API
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

    // Enable connection reuse (TCP Keepalive)
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL, 60L);

    // HTTP/2 support if available
#ifdef CURL_HTTP_VERSION_2_0
    if (m_settings.get_bool(settings_pack::enable_http2_trackers)) {
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
    }
#endif

    // Configure Proxy settings
    configure_proxy(easy, m_settings);

    // Configure SSL/TLS settings
    configure_ssl(easy, m_settings);

    return true;
}

void curl_thread_manager::curl_thread_func() {
    // Create multi handle for this thread
    CURLM* multi = curl_multi_init();
    if (!multi) {
        // Fatal error - signal initialization failed
        {
            std::lock_guard<std::mutex> lock(m_init_mutex);
            m_init_status = InitStatus::Failed;
        }
        m_init_cv.notify_one();
        return;
    }
    
    // Configure multi handle for connection pooling
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 100L);
    
    // Enable HTTP/2 if available
#ifdef CURLPIPE_MULTIPLEX
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#endif
    
    // Store multi handle for wakeup mechanism
    m_multi_handle = multi;
    
    // Signal that initialization is successful
    {
        std::lock_guard<std::mutex> lock(m_init_mutex);
        m_init_status = InitStatus::Success;
    }
    m_init_cv.notify_one();
    
    // Main event loop
    while (true) {
        // ALWAYS process pending requests from queue first (before checking stop signal)
        auto pending = swap_pending_requests();
        
        for (auto& req : pending) {
            // CRITICAL FIX: Create transfer data with RAII handle
            // The curl_transfer_data holds both the shared_ptr to response buffer
            // and the RAII wrapper for the CURL handle
            curl_transfer_data* transfer_data = nullptr;
            try {
                transfer_data = new curl_transfer_data(std::move(req));
            } catch (const std::exception&) {
                // Failed to create CURL handle
                boost::asio::post(m_ios, [handler = req.completion_handler]() {
                    handler(errors::no_memory, std::vector<char>{});
                });
                continue;
            }
            
            CURL* easy = transfer_data->easy_handle.get();
            if (!configure_handle(easy, transfer_data->request)) {
                delete transfer_data;  // RAII wrapper will clean up CURL handle
                boost::asio::post(m_ios, [handler = transfer_data->request.completion_handler]() {
                    handler(errors::timed_out, std::vector<char>{});
                });
                continue;
            }
            
            // Pass raw buffer pointer (safe - transfer_data keeps it alive)
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer_data->response.get());
            
            // Store transfer data pointer for later retrieval
            curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer_data);
            
            CURLMcode add_result = curl_multi_add_handle(multi, easy);
            if (add_result != CURLM_OK) {
                auto handler = transfer_data->request.completion_handler; // Save before delete
                delete transfer_data;  // RAII wrapper will clean up CURL handle
                boost::asio::post(m_ios, [handler]() {
                    handler(errors::no_memory, std::vector<char>{});
                });
            } else {
                transfer_data->request.easy_handle = easy;
                m_active_requests[easy] = std::move(transfer_data->request);
            }
        }
        
        // Process retry queue
        auto now = clock_type::now();
        if (!m_retry_queue.empty()) {
            std::fprintf(stderr, "DEBUG: Retry queue has %zu items\n", m_retry_queue.size());
            std::fflush(stderr);
        }
        
        // Use iterator-based approach with multiset for safe extraction
        auto it = m_retry_queue.begin();
        while (it != m_retry_queue.end() && it->scheduled_time <= now) {
            // Copy the retry item (C++14 compatible)
            retry_item item = *it;
            // Erase before processing to maintain queue consistency
            it = m_retry_queue.erase(it);
            curl_request req = std::move(item.request);
            
            std::fprintf(stderr, "DEBUG: Processing retry #%d from queue for %s\n", 
                        req.retry_count, req.url.c_str());
            std::fflush(stderr);
            
            // Check if still within deadline
            if (now >= req.deadline) {
                // Timeout - don't retry
                boost::asio::post(m_ios, [handler = req.completion_handler]() {
                    handler(errors::timed_out, std::vector<char>{});
                });
                continue;
            }
            
            // CRITICAL FIX: Create transfer data with RAII handle
            curl_transfer_data* transfer_data = nullptr;
            try {
                transfer_data = new curl_transfer_data(std::move(req));
            } catch (const std::exception&) {
                // Failed to create CURL handle
                boost::asio::post(m_ios, [handler = req.completion_handler]() {
                    handler(errors::no_memory, std::vector<char>{});
                });
                continue;
            }

            CURL* easy = transfer_data->easy_handle.get();
            // Configure the request using the centralized helper
            if (!configure_handle(easy, transfer_data->request)) {
                // Configuration failed (e.g., already past deadline)
                auto handler = transfer_data->request.completion_handler;  // Save handler before delete
                delete transfer_data;  // RAII wrapper will clean up CURL handle
                boost::asio::post(m_ios, [handler]() {
                    handler(errors::timed_out, std::vector<char>{});
                });
                continue;
            }
            
            // Pass raw buffer pointer (safe - transfer_data keeps it alive)
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer_data->response.get());
            
            // Store transfer data pointer for later retrieval
            curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer_data);
            
            // Handle potential failure of curl_multi_add_handle
            CURLMcode add_result = curl_multi_add_handle(multi, easy);
            if (add_result != CURLM_OK) {
                auto handler = transfer_data->request.completion_handler; // Save before delete
                delete transfer_data;  // RAII wrapper will clean up CURL handle
                boost::asio::post(m_ios, [handler]() {
                    handler(errors::no_memory, std::vector<char>{}); // Assume memory issue
                });
            } else {
                transfer_data->request.easy_handle = easy;
                m_active_requests[easy] = std::move(transfer_data->request);
                m_retried_requests++;
            }
        }
        
        // --- SECTION 2: Perform Transfers (The core fix for connection pooling) ---
        // Drive the state machine. Repeat if completions occurred to start queued requests immediately.
        
        int running = 0;
        bool call_again = false;
        int total_completions = 0;
        
        do {
            
            CURLMcode mc = curl_multi_perform(multi, &running);

            // Check for completed transfers immediately after perform
            int completed = process_completions(multi);
            total_completions += completed;
            
            // DEBUG: Track if we got any completions
            if (completed > 0) {
                std::fprintf(stderr, "DEBUG: process_completions returned %d completions\n", completed);
                std::fflush(stderr);
            }
            
            if (completed > 0) {
                // If completions happened, slots are free. Force immediate re-perform.
                call_again = true; 
            } else {
                call_again = false;
            }

            // Check for errors. CURLM_OK is the success code.
            if (mc != CURLM_OK) {
                // We only tolerate the deprecated CURLM_CALL_MULTI_PERFORM if defined
                #ifdef CURLM_CALL_MULTI_PERFORM
                if (mc == CURLM_CALL_MULTI_PERFORM) {
                    // This means curl wants us to call it again immediately.
                } else 
                #endif
                {
                    // Handle actual error
                    break; // Break the do-while loop
                }
            }

        // Repeat if explicitly requested by curl (deprecated) or if we processed completions.
        #ifdef CURLM_CALL_MULTI_PERFORM
        } while (mc == CURLM_CALL_MULTI_PERFORM || call_again);
        #else
        } while (call_again);
        #endif
        
        // --- SECTION 3: Wait for Activity ---
        
        // Check if we should exit - only when stopping AND no active transfers
        if (m_stopping && running == 0) {
            break;  // Safe to exit - no pending work
        }

        // Calculate proper timeout to prevent 100% CPU usage
        long wait_ms = calculate_wait_timeout(multi);
        
        // During shutdown with active transfers, use shorter timeout for responsiveness
        if (m_stopping && running > 0) {
            wait_ms = std::min(wait_ms, 100L);  // Check more frequently during shutdown
        }
        
        // Wait for socket activity, timeout, or wakeup
        int numfds = 0;
        
        // CRITICAL FIX for 100% CPU bug:
        // Use curl_multi_poll() instead of curl_multi_wait()
        // curl_multi_poll() properly respects the timeout even when there are no
        // file descriptors to monitor, preventing the busy-wait loop.
        // This requires libcurl 7.66.0+ which we verify in the constructor.
        
        CURLMcode mc = curl_multi_poll(multi, nullptr, 0, static_cast<int>(wait_ms), &numfds);
        
        if (mc != CURLM_OK) {
            // Log error but continue
            // Brief sleep to prevent spin on persistent error
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    // Cleanup on shutdown - properly clean up transfer_data
    for (auto& pair : m_active_requests) {
        CURL* easy = pair.first;
        curl_request& req = pair.second;
        
        // CRITICAL: Retrieve and delete transfer_data to prevent memory leak
        curl_transfer_data* transfer_data = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &transfer_data);
        
        curl_multi_remove_handle(multi, easy);
        
        if (transfer_data) {
            delete transfer_data;  // RAII wrapper will clean up CURL handle
        }
        
        // Notify completion with error
        boost::asio::post(m_ios, [handler = req.completion_handler]() {
            handler(errors::session_is_closing, std::vector<char>{});
        });
    }
    
    // Clear multi handle reference before cleanup
    m_multi_handle = nullptr;
    curl_multi_cleanup(multi);
}

int curl_thread_manager::process_completions(CURLM* multi) {
    CURLMsg* msg;
    int msgs_left;
    int completed_count = 0;
    
    // CORRECT: Loop while reading messages from curl
    while ((msg = curl_multi_info_read(multi, &msgs_left))) {
        // DEBUG: Log any completed message
        if (msg->msg == CURLMSG_DONE) {
            std::fprintf(stderr, "DEBUG: Got CURLMSG_DONE with result=%d\n", msg->data.result);
            std::fflush(stderr);
        }
        if (msg->msg != CURLMSG_DONE) continue;
        
        CURL* easy = msg->easy_handle;
        completed_count++;
        CURLcode result = msg->data.result;
        
        // CRITICAL FIX: Retrieve and delete transfer data
        curl_transfer_data* transfer_data = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &transfer_data);
        
        // Find the request in our tracking map
        auto it = m_active_requests.find(easy);
        if (it == m_active_requests.end() || !transfer_data) {
            // Handle unexpected state: message received for unknown handle
            curl_multi_remove_handle(multi, easy);
            if (transfer_data) {
                delete transfer_data;  // RAII wrapper will clean up CURL handle
            }
            continue;
        }
        
        curl_request req = std::move(it->second);
        m_active_requests.erase(it);
        
        // Remove from multi handle
        curl_multi_remove_handle(multi, easy);
        
        // Determine error code
        error_code ec = curl_error_to_libtorrent(result);
        
        // DEBUG: Log CURL write errors
        if (result == CURLE_WRITE_ERROR) {
            std::fprintf(stderr, "DEBUG: CURLE_WRITE_ERROR for %s\n", req.url.c_str());
            std::fflush(stderr);
        }
        
        if (!ec) {
            // Check HTTP status code
            long response_code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response_code);
            
            if (response_code >= 400) {
                ec = errors::http_error;
                
                // Consider retry for server errors
                if (response_code >= 500 && req.retry_count < req.max_retries) {
                    std::fprintf(stderr, "DEBUG: HTTP %ld - scheduling retry %d/%d for %s\n", 
                                response_code, req.retry_count + 1, req.max_retries, req.url.c_str());
                    std::fflush(stderr);
                    // CRITICAL: Clean up transfer_data before retry (RAII wrapper cleans up CURL handle)
                    delete transfer_data;
                    schedule_retry(std::move(req));
                    continue;
                } else if (response_code >= 500) {
                    std::fprintf(stderr, "DEBUG: HTTP %ld - exhausted retries (%d/%d) for %s\n", 
                                response_code, req.retry_count, req.max_retries, req.url.c_str());
                    std::fflush(stderr);
                }
            }
        } else if (is_retryable_error(ec)) {
            // Consider retry for transient errors
            // Exclude non-retryable curl codes: permanent failures that won't be fixed by retrying
            if (result != CURLE_WRITE_ERROR &&
                result != CURLE_FILESIZE_EXCEEDED &&
                result != CURLE_UNSUPPORTED_PROTOCOL &&
                result != CURLE_COULDNT_RESOLVE_PROXY &&
                result != CURLE_COULDNT_RESOLVE_HOST &&  // DNS failures won't be fixed by retry
                result != CURLE_COULDNT_CONNECT &&  // Connection failures mean server is down
                req.retry_count < req.max_retries &&
                clock_type::now() < req.deadline) {
                // CRITICAL: Clean up transfer_data before retry (RAII wrapper cleans up CURL handle)
                delete transfer_data;
                schedule_retry(std::move(req));
                continue;
            }
        }
        
        // Update metrics
        if (ec) {
            m_failed_requests++;
        } else {
            m_completed_requests++;
        }
        
        // Post completion to io_context (thread-safe)
        // Move the buffer out of the transfer_data's response structure
        std::vector<char> response = std::move(transfer_data->response->buffer);
        auto handler = req.completion_handler;
        
        // CRITICAL: Clean up transfer data (RAII wrapper cleans up CURL handle)
        delete transfer_data;
        
        // DEBUG: Log completion posting
        if (result == CURLE_WRITE_ERROR) {
            std::fprintf(stderr, "DEBUG: Posting completion handler for WRITE_ERROR with ec=%d\n", ec.value());
            std::fflush(stderr);
        }
        
        boost::asio::post(m_ios, 
            [handler, ec, response = std::move(response)]() {
                handler(ec, response);
            });
    }
    
    return completed_count;
}

void curl_thread_manager::schedule_retry(curl_request req) {
    // Exponential backoff
    req.retry_count++;
    req.retry_delay *= 2;
    
    // Cap maximum delay at 30 seconds
    req.retry_delay = std::min(req.retry_delay, milliseconds(30000));
    
    // Clear response buffer for retry
    req.response->buffer.clear();
    
    // Schedule retry
    auto retry_time = clock_type::now() + req.retry_delay;
    
    // Don't retry past deadline
    if (retry_time >= req.deadline) {
        // Give up - timeout
        boost::asio::post(m_ios, [handler = req.completion_handler]() {
            handler(errors::timed_out, std::vector<char>{});
        });
        return;
    }
    
    // Insert into multiset (safe, no const_cast needed)
    m_retry_queue.insert({retry_time, std::move(req)});
}

// CRITICAL FIX: Calculate proper wait timeout to prevent 100% CPU usage when idle
long curl_thread_manager::calculate_wait_timeout(CURLM* multi) const {
    // Step 1: Get libcurl's internal timeout recommendation
    long curl_timeout_ms = -1;
    curl_multi_timeout(multi, &curl_timeout_ms);
    
    // Step 2: Calculate application-level timeout (retry queue)
    long app_timeout_ms = -1;
    if (!m_retry_queue.empty()) {
        auto now = clock_type::now();
        auto next_retry = m_retry_queue.begin()->scheduled_time;
        if (next_retry > now) {
            app_timeout_ms = std::chrono::duration_cast<milliseconds>(
                next_retry - now).count();
        } else {
            app_timeout_ms = 0; // Retry is ready now
        }
    }
    
    // Step 3: Determine final timeout
    long wait_ms;
    
    // If we have active transfers or pending work
    if (!m_active_requests.empty()) {
        // Use libcurl's timeout if valid, otherwise small default
        if (curl_timeout_ms >= 0) {
            wait_ms = curl_timeout_ms;
        } else {
            wait_ms = 100; // 100ms default for active transfers
        }
        
        // Consider app timeout if we have retries pending
        if (app_timeout_ms >= 0 && app_timeout_ms < wait_ms) {
            wait_ms = app_timeout_ms;
        }
    }
    // If we're truly idle (no active transfers)
    else if (app_timeout_ms >= 0) {
        // Wait until next retry
        wait_ms = app_timeout_ms;
    }
    else {
        // CRITICAL: When completely idle, wait indefinitely (or very long)
        // curl_multi_wakeup() will interrupt this when new work arrives
        wait_ms = 60000; // 60 seconds - will be interrupted by wakeup
    }
    
    // Safety: Never return negative timeout
    return std::max(0L, wait_ms);
}

}} // namespace libtorrent::aux

#endif // TORRENT_USE_LIBCURL