/**
 * NTONIX - High-Performance AI Inference Gateway
 * Server component - Async I/O foundation with graceful shutdown
 */

#ifndef NTONIX_SERVER_SERVER_HPP
#define NTONIX_SERVER_SERVER_HPP

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace ntonix::server {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

/**
 * Server configuration for async I/O foundation
 */
struct ServerConfig {
    std::uint16_t port{8080};
    std::size_t thread_count{std::thread::hardware_concurrency()};
    std::string bind_address{"0.0.0.0"};
};

/**
 * Connection handler type - called when a new connection is accepted
 */
using ConnectionHandler = std::function<void(tcp::socket)>;

/**
 * Main server class - manages io_context, thread pool, and TCP acceptor
 *
 * Uses std::jthread with stop_token for graceful shutdown.
 * Signal handling is integrated for clean termination on SIGINT/SIGTERM.
 */
class Server {
public:
    explicit Server(const ServerConfig& config);
    ~Server();

    // Non-copyable, non-movable (owns threads and io_context)
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /**
     * Start the server - begins accepting connections
     * @param handler Callback invoked for each accepted connection
     */
    void start(ConnectionHandler handler);

    /**
     * Request graceful shutdown
     * Stops accepting new connections and waits for active work to complete.
     */
    void stop();

    /**
     * Block until server stops
     */
    void wait();

    /**
     * Check if server is running
     */
    bool is_running() const noexcept;

    /**
     * Get the io_context (for scheduling async work)
     */
    asio::io_context& get_io_context() noexcept;

    /**
     * Get the port the server is listening on
     */
    std::uint16_t get_port() const noexcept;

private:
    void run_io_context(std::stop_token stop_token);
    void do_accept();
    void setup_signal_handling();

    ServerConfig config_;
    asio::io_context io_context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    tcp::acceptor acceptor_;
    asio::signal_set signals_;

    std::vector<std::jthread> thread_pool_;
    ConnectionHandler connection_handler_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> connections_accepted_{0};
};

} // namespace ntonix::server

#endif // NTONIX_SERVER_SERVER_HPP
