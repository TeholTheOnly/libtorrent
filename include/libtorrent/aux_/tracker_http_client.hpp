#ifndef TORRENT_TRACKER_HTTP_CLIENT_HPP
#define TORRENT_TRACKER_HTTP_CLIENT_HPP

#include "libtorrent/config.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/settings_pack.hpp"
#include "libtorrent/tracker_manager.hpp"
#include <memory>
#include <functional>

namespace libtorrent { namespace aux {

// Abstract interface for HTTP tracker communication
// Supports both HTTP/1.1 and HTTP/2 implementations
class TORRENT_EXPORT tracker_http_client {
public:
    virtual ~tracker_http_client() = default;

    // Announce to tracker
    // @param req The tracker request containing announce parameters
    // @param handler Callback invoked with error code and response
    virtual void announce(
        tracker_request const& req,
        std::function<void(error_code const&, tracker_response const&)> handler
    ) = 0;

    // Scrape tracker for torrent statistics
    // @param req The tracker request containing scrape parameters
    // @param handler Callback invoked with error code and response
    virtual void scrape(
        tracker_request const& req,
        std::function<void(error_code const&, tracker_response const&)> handler
    ) = 0;

    // Check if connection can be reused for another request
    // @return true if connection is active and reusable
    virtual bool can_reuse() const = 0;

    // Close all connections and cleanup resources
    virtual void close() = 0;
};

// Factory function to create appropriate client based on URL and settings
// @param ios The IO context for async operations
// @param url The tracker URL (determines protocol)
// @param settings Configuration including HTTP/2 enable flag
// @return Unique pointer to appropriate client implementation
TORRENT_EXPORT std::unique_ptr<tracker_http_client> create_tracker_client(
    io_context& ios,
    std::string const& url,
    settings_pack const& settings
);

}} // namespace libtorrent::aux

#endif // TORRENT_TRACKER_HTTP_CLIENT_HPP