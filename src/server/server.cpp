/**
 * NTONIX - High-Performance AI Inference Gateway
 * Server implementation - Async I/O foundation with graceful shutdown
 */

#include "server/server.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

namespace ntonix::server {

Server::Server(const ServerConfig& config)
    : config_(config)
    , io_context_(static_cast<int>(config.thread_count))
    , work_guard_(asio::make_work_guard(io_context_))
    , acceptor_(io_context_)
    , signals_(io_context_)
{
    spdlog::debug("Server: Initializing with {} threads on {}:{}",
                  config_.thread_count, config_.bind_address, config_.port);
}

Server::~Server() {
    stop();
    wait();
}

void Server::start(ConnectionHandler handler, ReloadHandler reload_handler) {
    if (running_.exchange(true)) {
        spdlog::warn("Server: Already running, ignoring start request");
        return;
    }

    connection_handler_ = std::move(handler);
    reload_handler_ = std::move(reload_handler);

    // Setup signal handling for graceful shutdown
    setup_signal_handling();

    // Configure and open the acceptor
    tcp::endpoint endpoint(
        asio::ip::make_address(config_.bind_address),
        config_.port
    );

    boost::system::error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        spdlog::error("Server: Failed to open acceptor: {}", ec.message());
        running_ = false;
        throw std::runtime_error("Failed to open acceptor: " + ec.message());
    }

    // Set socket options
    acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) {
        spdlog::warn("Server: Failed to set reuse_address: {}", ec.message());
    }

    acceptor_.bind(endpoint, ec);
    if (ec) {
        spdlog::error("Server: Failed to bind to {}:{}: {}",
                      config_.bind_address, config_.port, ec.message());
        running_ = false;
        throw std::runtime_error("Failed to bind: " + ec.message());
    }

    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        spdlog::error("Server: Failed to listen: {}", ec.message());
        running_ = false;
        throw std::runtime_error("Failed to listen: " + ec.message());
    }

    spdlog::info("Server: Listening on {}:{}", config_.bind_address, config_.port);

    // Start accepting connections
    do_accept();

    // Start worker threads
    thread_pool_.reserve(config_.thread_count);
    for (std::size_t i = 0; i < config_.thread_count; ++i) {
        thread_pool_.emplace_back([this](std::stop_token st) {
            run_io_context(st);
        });
    }

    spdlog::info("Server: Started with {} worker threads", config_.thread_count);
}

void Server::stop() {
    if (!running_.exchange(false)) {
        return; // Already stopped
    }

    spdlog::info("Server: Initiating graceful shutdown...");

    // Stop accepting new connections
    boost::system::error_code ec;
    acceptor_.close(ec);
    if (ec) {
        spdlog::warn("Server: Error closing acceptor: {}", ec.message());
    }

    // Cancel signal handling
    signals_.cancel(ec);

    // Release the work guard to allow io_context to complete
    work_guard_.reset();

    // Request stop on all threads
    for (auto& thread : thread_pool_) {
        thread.request_stop();
    }

    // Stop io_context (interrupts any waiting async operations)
    io_context_.stop();

    spdlog::info("Server: Shutdown initiated, waiting for threads...");
}

void Server::wait() {
    for (auto& thread : thread_pool_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    thread_pool_.clear();
    spdlog::info("Server: All worker threads terminated");
}

bool Server::is_running() const noexcept {
    return running_.load();
}

asio::io_context& Server::get_io_context() noexcept {
    return io_context_;
}

std::uint16_t Server::get_port() const noexcept {
    return config_.port;
}

void Server::run_io_context(std::stop_token stop_token) {
    spdlog::debug("Server: Worker thread started");

    while (!stop_token.stop_requested()) {
        try {
            io_context_.run();
            break; // Normal exit when io_context runs out of work
        } catch (const std::exception& e) {
            spdlog::error("Server: Exception in worker thread: {}", e.what());
        }
    }

    spdlog::debug("Server: Worker thread exiting");
}

void Server::do_accept() {
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
                    spdlog::error("Server: Accept error: {}", ec.message());
                }
                // Continue accepting unless stopped
                if (running_ && ec != asio::error::operation_aborted) {
                    do_accept();
                }
                return;
            }

            ++connections_accepted_;
            auto remote = socket.remote_endpoint(ec);
            if (!ec) {
                spdlog::info("Server: Connection #{} accepted from {}:{}",
                            connections_accepted_.load(),
                            remote.address().to_string(),
                            remote.port());
            } else {
                spdlog::info("Server: Connection #{} accepted",
                            connections_accepted_.load());
            }

            // Invoke the connection handler
            if (connection_handler_) {
                try {
                    connection_handler_(std::move(socket));
                } catch (const std::exception& e) {
                    spdlog::error("Server: Connection handler exception: {}", e.what());
                }
            }

            // Continue accepting
            do_accept();
        }
    );
}

void Server::setup_signal_handling() {
    // Handle SIGINT (Ctrl+C) and SIGTERM for shutdown
    signals_.add(SIGINT);
    signals_.add(SIGTERM);
#ifndef _WIN32
    // SIGHUP for config reload (Unix only)
    signals_.add(SIGHUP);
#endif

    auto handle_signal = [this](boost::system::error_code ec, int signal_number) {
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                spdlog::debug("Server: Signal handler error: {}", ec.message());
            }
            return;
        }

#ifndef _WIN32
        // SIGHUP triggers config reload, not shutdown
        if (signal_number == SIGHUP) {
            spdlog::info("Server: Received SIGHUP - reloading configuration");
            if (reload_handler_) {
                try {
                    reload_handler_();
                } catch (const std::exception& e) {
                    spdlog::error("Server: Config reload failed: {}", e.what());
                }
            } else {
                spdlog::warn("Server: No reload handler configured, ignoring SIGHUP");
            }
            // Re-register for next signal
            setup_signal_handling();
            return;
        }
#endif

        spdlog::info("Server: Received signal {} - initiating shutdown", signal_number);
        stop();
    };

    signals_.async_wait(handle_signal);

    spdlog::debug("Server: Signal handlers installed (SIGINT, SIGTERM, SIGHUP)");
}

} // namespace ntonix::server
