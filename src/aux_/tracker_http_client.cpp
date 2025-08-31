#include "libtorrent/aux_/tracker_http_client.hpp"
#ifdef TORRENT_USE_LIBCURL
#include "libtorrent/aux_/curl_tracker_client.hpp"
#else
#include "libtorrent/aux_/http1_tracker_client.hpp"  // To be implemented
#endif
#include "libtorrent/parse_url.hpp"

namespace libtorrent { namespace aux {

std::unique_ptr<tracker_http_client> create_tracker_client(
    io_context& ios,
    std::string const& url,
    settings_pack const& settings)
{
    // Parse URL to determine protocol
    error_code ec;
    std::string protocol, auth, hostname, path;
    int port;
    std::tie(protocol, auth, hostname, port, path) = parse_url_components(url, ec);
    
    if (ec) {
        // Return null for invalid URLs
        return nullptr;
    }

#ifdef TORRENT_USE_LIBCURL
    // When libcurl is available, always use it for both HTTP/1.1 and HTTP/2
    // libcurl handles protocol negotiation and fallback automatically
    return std::make_unique<curl_tracker_client>(ios, url, settings);
#else
    // Fall back to the basic HTTP/1.1 implementation
    return std::make_unique<http1_tracker_client>(ios, url, settings);
#endif
}

}} // namespace libtorrent::aux