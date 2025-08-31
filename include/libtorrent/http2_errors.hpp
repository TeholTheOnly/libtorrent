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

#ifndef TORRENT_HTTP2_ERRORS_HPP
#define TORRENT_HTTP2_ERRORS_HPP

#include "libtorrent/config.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/aux_/export.hpp"
#include <boost/system/error_code.hpp>

namespace libtorrent { namespace errors {

    // HTTP/2 specific error codes
    // Use high range (10000+) to avoid conflicts with system error codes
    enum http2_errors {
        // Connection errors
        http2_alpn_negotiation_failed = 10000,
        http2_no_available_session = 10001,
        http2_goaway_received = 10002,
        http2_proxy_connect_failed = 10003,
        http2_protocol_error = 10004,
        http2_stream_limit_exceeded = 10005,
        http2_session_closing = 10006,
        
        // Timeout errors
        http2_timed_out = 10007,
        http2_stream_timeout = 10008,
        
        // Pool management errors
        http2_pool_size_exceeded = 10009,
        http2_too_many_pending_operations = 10010,
        
        // HTTP status codes (offset by 10000 for uniqueness)
        http_400_bad_request = 10400,
        http_401_unauthorized = 10401,
        http_403_forbidden = 10403,
        http_404_not_found = 10404,
        http_408_request_timeout = 10408,
        http_429_too_many_requests = 10429,
        http_500_internal_server_error = 10500,
        http_502_bad_gateway = 10502,
        http_503_service_unavailable = 10503
    };

    // Error category for HTTP/2 specific errors
    struct TORRENT_EXPORT http2_error_category : boost::system::error_category {
        const char* name() const BOOST_SYSTEM_NOEXCEPT override;
        std::string message(int ev) const override;
        
        // Singleton pattern for category
        static http2_error_category& instance();
    };

    // Helper function to create error codes
    inline boost::system::error_code make_error_code(http2_errors e) {
        return boost::system::error_code(
            static_cast<int>(e), 
            http2_error_category::instance()
        );
    }

}} // namespace libtorrent::errors

// Register with boost::system
namespace boost { namespace system {
    template<> struct is_error_code_enum<libtorrent::errors::http2_errors> {
        static const bool value = true;
    };
}} // namespace boost::system

#endif // TORRENT_HTTP2_ERRORS_HPP