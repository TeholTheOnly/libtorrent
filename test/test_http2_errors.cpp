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

#include "test.hpp"
#include "libtorrent/http2_errors.hpp"
#include <set>

using namespace libtorrent;
using namespace libtorrent::errors;

TORRENT_TEST(http2_error_code_creation)
{
    error_code ec = make_error_code(errors::http2_alpn_negotiation_failed);
    TEST_CHECK(ec);  // Error condition exists
    TEST_EQUAL(ec.value(), static_cast<int>(errors::http2_alpn_negotiation_failed));
    TEST_EQUAL(ec.category().name(), std::string("http2"));
}

TORRENT_TEST(http2_error_messages)
{
    struct test_case {
        http2_errors code;
        const char* expected_message;
    };
    
    test_case cases[] = {
        {http2_alpn_negotiation_failed, "ALPN negotiation failed - server doesn't support HTTP/2"},
        {http2_no_available_session, "No HTTP/2 session available with stream capacity"},
        {http2_goaway_received, "Server sent GOAWAY frame"},
        {http2_proxy_connect_failed, "Failed to establish proxy CONNECT tunnel"},
        {http2_stream_limit_exceeded, "Maximum concurrent streams exceeded"},
        {http2_stream_timeout, "Stream request timed out"},
        {http_400_bad_request, "HTTP 400 Bad Request"},
        {http_429_too_many_requests, "HTTP 429 Too Many Requests"},
        {http_503_service_unavailable, "HTTP 503 Service Unavailable"}
    };
    
    for (auto const& tc : cases) {
        error_code ec = make_error_code(tc.code);
        TEST_EQUAL(ec.message(), tc.expected_message);
    }
}

TORRENT_TEST(http2_error_code_uniqueness)
{
    std::set<int> error_values;
    
    int codes[] = {
        http2_alpn_negotiation_failed, http2_no_available_session, http2_goaway_received,
        http2_proxy_connect_failed, http2_protocol_error, http2_stream_limit_exceeded,
        http2_session_closing, http2_timed_out, http2_stream_timeout, http2_pool_size_exceeded,
        http2_too_many_pending_operations
    };
    
    for (int code : codes) {
        TEST_CHECK(code >= 10000);  // High range to avoid conflicts
        TEST_CHECK(error_values.insert(code).second);  // Unique insertion
    }
}

TORRENT_TEST(http2_status_code_errors)
{
    TEST_EQUAL(static_cast<int>(http_400_bad_request), 10400);
    TEST_EQUAL(static_cast<int>(http_401_unauthorized), 10401);
    TEST_EQUAL(static_cast<int>(http_403_forbidden), 10403);
    TEST_EQUAL(static_cast<int>(http_404_not_found), 10404);
    TEST_EQUAL(static_cast<int>(http_408_request_timeout), 10408);
    TEST_EQUAL(static_cast<int>(http_429_too_many_requests), 10429);
    TEST_EQUAL(static_cast<int>(http_500_internal_server_error), 10500);
    TEST_EQUAL(static_cast<int>(http_502_bad_gateway), 10502);
    TEST_EQUAL(static_cast<int>(http_503_service_unavailable), 10503);
}

TORRENT_TEST(http2_error_category_behavior)
{
    http2_error_category category;
    
    TEST_EQUAL(std::string(category.name()), "http2");
    
    TEST_EQUAL(category.message(99999), "Unknown HTTP/2 error");
    
    error_code ec1 = make_error_code(http2_alpn_negotiation_failed);
    error_code ec2 = make_error_code(http2_alpn_negotiation_failed);
    TEST_CHECK(ec1 == ec2);  // Same error codes should be equal
    TEST_CHECK(&ec1.category() == &ec2.category());  // Same category instance
}

TORRENT_TEST(http2_error_comparison)
{
    error_code ec1 = make_error_code(http2_alpn_negotiation_failed);
    error_code ec2 = make_error_code(http2_goaway_received);
    error_code ec3 = make_error_code(http2_alpn_negotiation_failed);
    
    TEST_CHECK(ec1 != ec2);  // Different errors
    TEST_CHECK(ec1 == ec3);  // Same error
    TEST_CHECK(ec1 == errors::http2_alpn_negotiation_failed);  // Direct comparison
}

TORRENT_TEST(http2_boost_system_integration)
{
    error_code ec;
    TEST_CHECK(!ec);  // Default is no error
    
    ec = make_error_code(http2_stream_timeout);
    TEST_CHECK(ec);  // Has error
    TEST_CHECK(ec.value() != 0);
    
    ec.clear();
    TEST_CHECK(!ec);
    TEST_EQUAL(ec.value(), 0);
}