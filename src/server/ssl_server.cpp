/**
 * NTONIX - High-Performance AI Inference Gateway
 * SSL Server implementation - HTTPS acceptor with TLS termination
 */

#include "server/ssl_server.hpp"

#include <spdlog/spdlog.h>

namespace ntonix::server {

SslServer::SslServer(asio::io_context& io_context, const SslServerConfig& config)
    : config_(config)
    , io_context_(io_context)
    , acceptor_(io_context)
{
    spdlog::debug("SSL Server: Initializing on {}:{}",
                  config_.bind_address, config_.port);

    // Initialize SSL context
    try {
        ssl_context_manager_ = std::make_unique<SslContextManager>(config_.ssl);
        spdlog::info("SSL Server: Certificate loaded - Subject: {}",
                    ssl_context_manager_->get_certificate_subject());
        spdlog::info("SSL Server: Certificate expires: {}",
                    ssl_context_manager_->get_certificate_expiry());
    } catch (const std::exception& e) {
        spdlog::error("SSL Server: Failed to initialize SSL context: {}", e.what());
        throw;
    }
}

SslServer::~SslServer() {
    stop();
}

void SslServer::start(SslConnectionHandler handler) {
    if (running_.exchange(true)) {
        spdlog::warn("SSL Server: Already running, ignoring start request");
        return;
    }

    connection_handler_ = std::move(handler);

    // Configure and open the acceptor
    tcp::endpoint endpoint(
        asio::ip::make_address(config_.bind_address),
        config_.port
    );

    boost::system::error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        spdlog::error("SSL Server: Failed to open acceptor: {}", ec.message());
        running_ = false;
        throw std::runtime_error("Failed to open SSL acceptor: " + ec.message());
    }

    // Set socket options
    acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) {
        spdlog::warn("SSL Server: Failed to set reuse_address: {}", ec.message());
    }

    acceptor_.bind(endpoint, ec);
    if (ec) {
        spdlog::error("SSL Server: Failed to bind to {}:{}: {}",
                      config_.bind_address, config_.port, ec.message());
        running_ = false;
        throw std::runtime_error("Failed to bind SSL server: " + ec.message());
    }

    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        spdlog::error("SSL Server: Failed to listen: {}", ec.message());
        running_ = false;
        throw std::runtime_error("Failed to listen on SSL server: " + ec.message());
    }

    spdlog::info("SSL Server: Listening on {}:{} (HTTPS)",
                config_.bind_address, config_.port);

    // Start accepting connections
    do_accept();
}

void SslServer::stop() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    spdlog::info("SSL Server: Stopping...");

    boost::system::error_code ec;
    acceptor_.close(ec);
    if (ec) {
        spdlog::warn("SSL Server: Error closing acceptor: {}", ec.message());
    }

    spdlog::info("SSL Server: Stopped");
}

bool SslServer::is_running() const noexcept {
    return running_.load();
}

std::uint16_t SslServer::get_port() const noexcept {
    return config_.port;
}

ssl::context& SslServer::get_ssl_context() noexcept {
    return ssl_context_manager_->get_context();
}

SslContextManager& SslServer::get_ssl_context_manager() noexcept {
    return *ssl_context_manager_;
}

void SslServer::add_sni_context(const std::string& hostname, const SslConfig& config) {
    ssl_context_manager_->add_sni_context(hostname, config);
}

void SslServer::do_accept() {
    if (!running_) {
        return;
    }

    acceptor_.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket) {
            if (!running_) {
                return;
            }

            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    spdlog::error("SSL Server: Accept error: {}", ec.message());
                }
                // Continue accepting unless stopped
                if (running_ && ec != asio::error::operation_aborted) {
                    do_accept();
                }
                return;
            }

            ++connections_accepted_;
            boost::system::error_code ep_ec;
            auto remote = socket.remote_endpoint(ep_ec);
            if (!ep_ec) {
                spdlog::info("SSL Server: Connection #{} accepted from {}:{} (starting TLS handshake)",
                            connections_accepted_.load(),
                            remote.address().to_string(),
                            remote.port());
            } else {
                spdlog::info("SSL Server: Connection #{} accepted (starting TLS handshake)",
                            connections_accepted_.load());
            }

            // Invoke the connection handler with SSL context
            if (connection_handler_) {
                try {
                    connection_handler_(std::move(socket), ssl_context_manager_->get_context());
                } catch (const std::exception& e) {
                    spdlog::error("SSL Server: Connection handler exception: {}", e.what());
                }
            }

            // Continue accepting
            do_accept();
        }
    );
}

} // namespace ntonix::server
