/*

Copyright (c) 2025, Development Team
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

#include "libtorrent/http2_errors.hpp"

namespace libtorrent { namespace errors {

const char* http2_error_category::name() const BOOST_SYSTEM_NOEXCEPT {
    return "http2";
}

std::string http2_error_category::message(int ev) const {
    switch(static_cast<http2_errors>(ev)) {
        // Connection errors
        case http2_alpn_negotiation_failed:
            return "ALPN negotiation failed - server doesn't support HTTP/2";
        case http2_no_available_session:
            return "No HTTP/2 session available with stream capacity";
        case http2_goaway_received:
            return "Server sent GOAWAY frame";
        case http2_proxy_connect_failed:
            return "Failed to establish proxy CONNECT tunnel";
        case http2_protocol_error:
            return "HTTP/2 protocol error";
        case http2_stream_limit_exceeded:
            return "Maximum concurrent streams exceeded";
        case http2_session_closing:
            return "HTTP/2 session is closing";
            
        // Timeout errors
        case http2_timed_out:
            return "Operation timed out";
        case http2_stream_timeout:
            return "Stream request timed out";
            
        // Pool management errors
        case http2_pool_size_exceeded:
            return "Connection pool size limit exceeded";
        case http2_too_many_pending_operations:
            return "Too many pending connection operations";
            
        // HTTP status codes
        case http_400_bad_request:
            return "HTTP 400 Bad Request";
        case http_401_unauthorized:
            return "HTTP 401 Unauthorized";
        case http_403_forbidden:
            return "HTTP 403 Forbidden";
        case http_404_not_found:
            return "HTTP 404 Not Found";
        case http_408_request_timeout:
            return "HTTP 408 Request Timeout";
        case http_429_too_many_requests:
            return "HTTP 429 Too Many Requests";
        case http_500_internal_server_error:
            return "HTTP 500 Internal Server Error";
        case http_502_bad_gateway:
            return "HTTP 502 Bad Gateway";
        case http_503_service_unavailable:
            return "HTTP 503 Service Unavailable";
            
        default:
            return "Unknown HTTP/2 error";
    }
}

http2_error_category& http2_error_category::instance() {
    static http2_error_category category;
    return category;
}

}} // namespace libtorrent::errors