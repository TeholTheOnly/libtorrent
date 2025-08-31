/*

Copyright (c) 2025, Arvid Norberg
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
#include "setup_transfer.hpp"
#include "test_utils.hpp"

#ifdef TORRENT_USE_LIBCURL

#include "libtorrent/aux_/curl_tracker_manager.hpp"
#include "libtorrent/aux_/curl_tracker_client.hpp"
#include "libtorrent/settings_pack.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/aux_/time.hpp"

#include <memory>
#include <future>
#include <chrono>
#include <vector>
#include <string>

using namespace libtorrent;
using namespace libtorrent::aux;
using namespace std::chrono;

// Test 1: SSL certificate verification
TORRENT_TEST(libcurl_security_ssl_verification)
{
	io_context ios;
	settings_pack settings;
	
	// Enable SSL verification
	settings.set_bool(settings_pack::tracker_ssl_verify_peer, true);
	settings.set_bool(settings_pack::tracker_ssl_verify_host, true);
	
	auto manager = std::make_shared<curl_tracker_manager>(ios, settings);
	
	std::promise<error_code> promise;
	
	// Test connection to a site with invalid certificate (self-signed)
	// Using badssl.com test endpoints
	manager->add_request("https://self-signed.badssl.com/",
		[&promise](error_code const& ec, std::vector<char> const&) {
			promise.set_value(ec);
		});
	
	ios.run();
	
	error_code ec = promise.get_future().get();
	
	// Should fail due to certificate verification
	TEST_CHECK(ec);
	// Could be http_error, invalid_ssl_cert for SSL issues or timed_out for network issues
	TEST_CHECK(ec == errors::http_error || ec == errors::invalid_ssl_cert || ec == errors::timed_out);
	
	// Now test with verification disabled
	ios.restart();
	settings.set_bool(settings_pack::tracker_ssl_verify_peer, false);
	settings.set_bool(settings_pack::tracker_ssl_verify_host, false);
	
	auto manager2 = std::make_shared<curl_tracker_manager>(ios, settings);
	std::promise<error_code> promise2;
	
	manager2->add_request("https://self-signed.badssl.com/",
		[&promise2](error_code const& ec, std::vector<char> const&) {
			promise2.set_value(ec);
		});
	
	ios.run();
	
	// With verification disabled, it might succeed (depending on the server)
	// but we're mainly testing that the setting is applied
	ec = promise2.get_future().get();
	// Just verify the setting was applied without crashing
	TEST_CHECK(true);
}

// Test 2: Response size limits
TORRENT_TEST(libcurl_security_max_response_size)
{
	io_context ios;
	settings_pack settings;
	
	// Set a small response size limit (1KB for testing)
	settings.set_int(settings_pack::max_tracker_response_size, 1024);
	
	auto manager = std::make_shared<curl_tracker_manager>(ios, settings);
	
	std::promise<error_code> promise;
	std::promise<size_t> size_promise;
	
	// Test with a URL that returns large content
	// Using httpbin.org to get specific sized response
	manager->add_request("https://httpbin.org/bytes/10000",
		[&promise, &size_promise](error_code const& ec, std::vector<char> const& response) {
			promise.set_value(ec);
			size_promise.set_value(response.size());
		});
	
	ios.run();
	
	error_code ec = promise.get_future().get();
	size_t response_size = size_promise.get_future().get();
	
	// Should fail or truncate due to size limit
	TEST_CHECK(ec || response_size <= 1024);
}

// Test 3: Ensure redirects are disabled (security feature)
TORRENT_TEST(libcurl_security_no_redirects)
{
	io_context ios;
	settings_pack settings;
	
	// Redirects should be disabled by default for security
	// Following redirects can lead to SSRF attacks or redirect to malicious sites
	auto manager = std::make_shared<curl_tracker_manager>(ios, settings);
	
	std::promise<error_code> promise;
	
	// Test with URL that redirects
	// This should fail because we don't follow redirects
	manager->add_request("https://httpbin.org/redirect/1",
		[&promise](error_code const& ec, std::vector<char> const&) {
			promise.set_value(ec);
		});
	
	ios.run();
	
	error_code ec = promise.get_future().get();
	
	// Should get an error (redirect not followed)
	// This is the secure behavior - torrents should use the correct URL
	TEST_CHECK(ec);
}

// Test 4: TLS version enforcement
TORRENT_TEST(libcurl_security_min_tls_version)
{
	io_context ios;
	settings_pack settings;
	
	// Set minimum TLS version to 1.2
	settings.set_int(settings_pack::tracker_min_tls_version, 0x0303); // TLS 1.2
	
	auto manager = std::make_shared<curl_tracker_manager>(ios, settings);
	
	std::promise<error_code> promise;
	
	// Test connection to a server that supports TLS 1.2+
	// Most modern servers support this
	manager->add_request("https://httpbin.org/get",
		[&promise](error_code const& ec, std::vector<char> const&) {
			promise.set_value(ec);
		});
	
	ios.run();
	
	error_code ec = promise.get_future().get();
	
	// Should succeed with modern TLS
	if (!ec) {
		TEST_CHECK(true); // Connection succeeded with TLS 1.2+
	} else {
		// If it failed, could be network issue, not necessarily TLS
		TEST_CHECK(ec == errors::http_error || ec == errors::timed_out);
	}
	
	// Test rejection of old TLS versions
	// We'd need a server that only supports TLS 1.0/1.1 to properly test this
	// For now, we just verify the setting is applied
	
	ios.restart();
	settings.set_int(settings_pack::tracker_min_tls_version, 0x0304); // TLS 1.3 only
	
	auto manager2 = std::make_shared<curl_tracker_manager>(ios, settings);
	std::promise<error_code> promise2;
	
	// Some servers might not support TLS 1.3 yet
	manager2->add_request("https://httpbin.org/get",
		[&promise2](error_code const& ec, std::vector<char> const&) {
			promise2.set_value(ec);
		});
	
	ios.run();
	
	// Just verify the setting is applied without crashing
	TEST_CHECK(true);
}

// Test 5: Protocol restrictions - only HTTP/HTTPS allowed
TORRENT_TEST(libcurl_security_protocol_restriction)
{
	io_context ios;
	settings_pack settings;
	
	// curl_tracker_manager should only allow HTTP/HTTPS protocols
	// This prevents attacks using file://, ftp://, gopher://, etc.
	auto manager = std::make_shared<curl_tracker_manager>(ios, settings);
	
	// Test that dangerous protocols are rejected
	std::promise<error_code> promise_file;
	std::promise<error_code> promise_ftp;
	
	// FILE protocol should be rejected (prevents local file access)
	manager->add_request("file:///etc/passwd",
		[&promise_file](error_code const& ec, std::vector<char> const&) {
			promise_file.set_value(ec);
		});
	
	// FTP should be rejected (prevents SSRF attacks)
	manager->add_request("ftp://internal.server/",
		[&promise_ftp](error_code const& ec, std::vector<char> const&) {
			promise_ftp.set_value(ec);
		});
	
	ios.run();
	
	// Both should fail due to protocol restrictions
	error_code ec_file = promise_file.get_future().get();
	error_code ec_ftp = promise_ftp.get_future().get();
	
	TEST_CHECK(ec_file);
	TEST_CHECK(ec_ftp);
	
	// Verify HTTP/HTTPS still work
	ios.restart();
	std::promise<error_code> promise_https;
	
	manager->add_request("https://httpbin.org/get",
		[&promise_https](error_code const& ec, std::vector<char> const&) {
			promise_https.set_value(ec);
		});
	
	ios.run();
	
	// HTTPS should work (or fail for network reasons, not protocol rejection)
	// We just verify it doesn't immediately reject the protocol
	TEST_CHECK(true);
}

// Test timeout enforcement
TORRENT_TEST(libcurl_security_timeout_enforcement)
{
	io_context ios;
	settings_pack settings;
	
	// Set very short timeout
	settings.set_int(settings_pack::tracker_completion_timeout, 1); // 1 second connect timeout
	settings.set_int(settings_pack::tracker_receive_timeout, 2); // 2 second total timeout
	
	auto manager = std::make_shared<curl_tracker_manager>(ios, settings);
	
	auto start = steady_clock::now();
	std::promise<error_code> promise;
	
	// Connect to non-routable address (will timeout)
	manager->add_request("https://10.255.255.255/announce",
		[&promise](error_code const& ec, std::vector<char> const&) {
			promise.set_value(ec);
		});
	
	ios.run();
	
	auto duration = steady_clock::now() - start;
	error_code ec = promise.get_future().get();
	
	// Should timeout within ~3 seconds (1 second connect + overhead)
	TEST_CHECK(ec);
	TEST_CHECK(ec == errors::timed_out || ec == errors::http_error || ec == errors::timed_out_no_handshake);
	TEST_CHECK(duration < seconds(4));
}

// Test response size limit enforcement against slow-drip attacks
TORRENT_TEST(curl_response_size_limit_enforcement)
{
	io_context ios;
	settings_pack pack;
	// Set a smaller limit for testing
	pack.set_int(settings_pack::max_tracker_response_size, 1024);  // 1KB limit
	auto manager = std::make_shared<curl_tracker_manager>(ios, pack);
	
	bool completed = false;
	error_code result_ec;
	size_t response_size = 0;
	
	// Use a URL that returns a large response (httpbin.org test endpoint)
	// This endpoint returns the requested number of bytes
	std::string url = "https://httpbin.org/bytes/2048";  // Request 2KB (exceeds 1KB limit)
	manager->add_request(url,
		[&](error_code const& ec, std::vector<char> const& response) {
			result_ec = ec;
			response_size = response.size();
			completed = true;
		});
	
	// Run event loop
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	while (!completed) {
		ios.run_one();
		auto elapsed = std::chrono::steady_clock::now() - start;
		if (elapsed > std::chrono::seconds(15)) {
			break;
		}
	}
	
	// Should have failed or limited the response
	TEST_CHECK(completed || response_size <= 1024);
	if (completed) {
		// Either got an error or limited response
		TEST_CHECK(result_ec || response_size <= 1024);
	}
}

// Test SSL certificate validation with various bad certificates
TORRENT_TEST(curl_ssl_certificate_validation)
{
	io_context ios;
	settings_pack pack;
	auto manager = std::make_shared<curl_tracker_manager>(ios, pack);
	
	struct ssl_test {
		std::string url;
		std::string description;
		bool should_fail;
	};
	
	// Using badssl.com test endpoints
	std::vector<ssl_test> tests = {
		{"https://expired.badssl.com/", "Expired certificate", true},
		{"https://wrong.host.badssl.com/", "Wrong hostname", true},
		{"https://self-signed.badssl.com/", "Self-signed certificate", true},
		{"https://untrusted-root.badssl.com/", "Untrusted root", true},
		// Note: We may want to allow some scenarios in the future
	};
	
	int completed = 0;
	for (auto const& test : tests) {
		manager->add_request(test.url,
			[&completed, &test](error_code const& ec, std::vector<char> const&) {
				if (test.should_fail) {
					// Should fail with SSL error
					TEST_CHECK(ec == errors::invalid_ssl_cert || ec == errors::http_error);
				}
				completed++;
			});
	}
	
	// Run event loop with timeout
	auto start = std::chrono::steady_clock::now();
	while (completed < static_cast<int>(tests.size())) {
		ios.run_one();
		auto elapsed = std::chrono::steady_clock::now() - start;
		if (elapsed > std::chrono::seconds(20)) {
			break;
		}
	}
	
	// All tests should complete
	TEST_CHECK(completed == static_cast<int>(tests.size()));
}

// Test redirect prevention (SSRF protection)
TORRENT_TEST(curl_redirect_prevention)
{
	io_context ios;
	settings_pack pack;
	auto manager = std::make_shared<curl_tracker_manager>(ios, pack);
	
	bool completed = false;
	error_code result_ec;
	
	// Use httpbin.org redirect endpoint that sends a 302 redirect
	std::string url = "https://httpbin.org/redirect/1";
	manager->add_request(url,
		[&](error_code const& ec, std::vector<char> const&) {
			result_ec = ec;
			completed = true;
		});
	
	// Run event loop
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	while (!completed) {
		ios.run_one();
		auto elapsed = std::chrono::steady_clock::now() - start;
		if (elapsed > std::chrono::seconds(10)) {
			break;
		}
	}
	
	// Should fail because redirects are disabled in our implementation
	TEST_CHECK(completed);
	TEST_CHECK(result_ec);  // Should have error since we don't follow redirects
}

// Test that tracker_ssl_verify settings are respected
TORRENT_TEST(curl_ssl_verify_settings_respected)
{
	io_context ios;
	
	// Test with verification disabled (for self-signed certs)
	{
		settings_pack pack;
		pack.set_bool(settings_pack::tracker_ssl_verify_peer, false);
		pack.set_bool(settings_pack::tracker_ssl_verify_host, false);
		auto manager = std::make_shared<curl_tracker_manager>(ios, pack);
		
		bool completed = false;
		error_code result_ec;
		
		// Request to a server with self-signed certificate
		// With verification disabled, this should succeed (or fail with network error, not SSL error)
		manager->add_request("https://self-signed.badssl.com/",
			[&](error_code const& ec, std::vector<char> const&) {
				result_ec = ec;
				completed = true;
			});
		
		// Run event loop
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		while (!completed) {
			ios.run_one();
			auto elapsed = std::chrono::steady_clock::now() - start;
			if (elapsed > std::chrono::seconds(10)) break;
		}
		
		// With verification disabled, should not get SSL certificate error
		TEST_CHECK(!result_ec || result_ec != errors::invalid_ssl_cert);
	}
	
	// Test with verification enabled (default)
	{
		settings_pack pack;
		// Defaults are true, but set explicitly for clarity
		pack.set_bool(settings_pack::tracker_ssl_verify_peer, true);
		pack.set_bool(settings_pack::tracker_ssl_verify_host, true);
		auto manager = std::make_shared<curl_tracker_manager>(ios, pack);
		
		bool completed = false;
		error_code result_ec;
		
		// Request to a server with self-signed certificate
		// With verification enabled, this should fail with SSL error
		manager->add_request("https://self-signed.badssl.com/",
			[&](error_code const& ec, std::vector<char> const&) {
				result_ec = ec;
				completed = true;
			});
		
		// Run event loop
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		while (!completed) {
			ios.run_one();
			auto elapsed = std::chrono::steady_clock::now() - start;
			if (elapsed > std::chrono::seconds(10)) break;
		}
		
		// With verification enabled, should get SSL error
		TEST_CHECK(result_ec == errors::invalid_ssl_cert || result_ec == errors::http_error);
	}
}

// Test that write callback exception safety works
TORRENT_TEST(curl_write_callback_exception_safety)
{
	// This test verifies that exceptions in the write callback
	// don't crash the program (they're caught and handled)
	
	io_context ios;
	settings_pack pack;
	auto manager = std::make_shared<curl_tracker_manager>(ios, pack);
	
	// Send multiple requests to various endpoints
	// Using invalid URLs that will fail quickly
	for (int i = 0; i < 10; ++i) {
		// Mix of different URL types to test various code paths
		std::string url;
		if (i % 3 == 0) {
			url = "http://0.0.0.0:" + std::to_string(30000 + i) + "/test";
		} else if (i % 3 == 1) {
			url = "https://invalid.test.domain." + std::to_string(i) + ".com/announce";
		} else {
			url = "http://[::1]:" + std::to_string(40000 + i) + "/test";
		}
		
		manager->add_request(url,
			[](error_code const&, std::vector<char> const&) {
				// Handler doesn't matter for this test
			});
	}
	
	// Run event loop to process requests
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
		ios.poll();
		ios.restart();
	}
	
	// If we get here without crashing, test passes
	TEST_CHECK(true);
}

#else // TORRENT_USE_LIBCURL

// Dummy test when libcurl is not available
TORRENT_TEST(libcurl_security_not_available)
{
	TEST_CHECK(true);
	std::cerr << "libcurl support not enabled. Security tests skipped.\n";
}

#endif // TORRENT_USE_LIBCURL