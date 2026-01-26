/**
 * NTONIX - High-Performance AI Inference Gateway
 * SSL Server component - HTTPS acceptor with TLS termination
 */

#ifndef NTONIX_SERVER_SSL_SERVER_HPP
#define NTONIX_SERVER_SSL_SERVER_HPP

#include "server/ssl_context.hpp"
#include "server/ssl_connection.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ntonix::server {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

/**
 * SSL Server configuration
 */
struct SslServerConfig {
    std::uint16_t port{8443};
    std::string bind_address{"0.0.0.0"};

    // SSL configuration
    SslConfig ssl;
};

/**
 * SSL Connection handler type - called when a new SSL connection is accepted
 */
using SslConnectionHandler = std::function<void(tcp::socket, ssl::context&)>;

/**
 * SSL Server class - manages HTTPS acceptor and TLS termination
 *
 * This server runs on a shared io_context and accepts SSL/TLS connections.
 * It handles TLS termination, allowing requests to be forwarded to backends
 * over plain HTTP.
 *
 * Key features:
 * - TLS 1.2 and TLS 1.3 support
 * - SNI (Server Name Indication) for virtual hosting
 * - Certificate and key loading from files
 * - Graceful SSL handshake error handling
 */
class SslServer {
public:
    /**
     * Create SSL server with existing io_context
     * @param io_context Shared io_context from main server
     * @param config SSL server configuration
     * @throws std::runtime_error if SSL initialization fails
     */
    SslServer(asio::io_context& io_context, const SslServerConfig& config);

    ~SslServer();

    // Non-copyable, non-movable
    SslServer(const SslServer&) = delete;
    SslServer& operator=(const SslServer&) = delete;
    SslServer(SslServer&&) = delete;
    SslServer& operator=(SslServer&&) = delete;

    /**
     * Start accepting HTTPS connections
     * @param handler Callback invoked for each accepted connection
     */
    void start(SslConnectionHandler handler);

    /**
     * Stop accepting new connections
     */
    void stop();

    /**
     * Check if server is running
     */
    bool is_running() const noexcept;

    /**
     * Get the port the server is listening on
     */
    std::uint16_t get_port() const noexcept;

    /**
     * Get the SSL context for advanced configuration
     */
    ssl::context& get_ssl_context() noexcept;

    /**
     * Get SSL context manager for certificate info
     */
    SslContextManager& get_ssl_context_manager() noexcept;

    /**
     * Add SNI context for a specific hostname
     */
    void add_sni_context(const std::string& hostname, const SslConfig& config);

private:
    void do_accept();

    SslServerConfig config_;
    asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::unique_ptr<SslContextManager> ssl_context_manager_;

    SslConnectionHandler connection_handler_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> connections_accepted_{0};
};

} // namespace ntonix::server

#endif // NTONIX_SERVER_SSL_SERVER_HPP
