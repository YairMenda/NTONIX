/**
 * NTONIX - High-Performance AI Inference Gateway
 * Backend Connection Pool - Reuse TCP connections to reduce handshake overhead
 */

#ifndef NTONIX_PROXY_CONNECTION_POOL_HPP
#define NTONIX_PROXY_CONNECTION_POOL_HPP

#include "config/config.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace ntonix::proxy {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

/**
 * Configuration for connection pool
 */
struct ConnectionPoolConfig {
    std::size_t pool_size_per_backend{10};              // Max connections per backend
    std::chrono::seconds idle_timeout{60};              // Close idle connections after this time
    std::chrono::seconds connection_timeout{5};         // Timeout for establishing connection
    std::chrono::seconds cleanup_interval{30};          // Interval for idle connection cleanup
    bool enable_keep_alive{true};                       // Enable TCP keep-alive
};

/**
 * A pooled connection to a backend
 */
class PooledConnection : public std::enable_shared_from_this<PooledConnection> {
public:
    using Ptr = std::shared_ptr<PooledConnection>;

    PooledConnection(tcp::socket socket, const config::BackendConfig& backend);
    ~PooledConnection();

    // Non-copyable but movable
    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    PooledConnection(PooledConnection&&) = default;
    PooledConnection& operator=(PooledConnection&&) = default;

    /**
     * Get the underlying socket
     */
    tcp::socket& socket() { return socket_; }
    const tcp::socket& socket() const { return socket_; }

    /**
     * Get the backend configuration this connection is for
     */
    const config::BackendConfig& backend() const { return backend_; }

    /**
     * Check if connection is still valid (socket is open)
     */
    bool is_valid() const;

    /**
     * Check if connection has been idle too long
     */
    bool is_idle(std::chrono::seconds max_idle) const;

    /**
     * Mark connection as in use (reset idle timer)
     */
    void mark_in_use();

    /**
     * Mark connection as returned to pool (start idle timer)
     */
    void mark_returned();

    /**
     * Get the time this connection has been idle
     */
    std::chrono::steady_clock::duration idle_time() const;

    /**
     * Get usage count (how many times this connection has been reused)
     */
    std::size_t usage_count() const { return usage_count_; }

private:
    tcp::socket socket_;
    config::BackendConfig backend_;
    std::chrono::steady_clock::time_point last_used_;
    std::size_t usage_count_{0};
    bool in_use_{false};
};

/**
 * RAII wrapper for checked-out connections
 * Automatically returns connection to pool on destruction
 */
class ConnectionGuard {
public:
    using ReleaseFunc = std::function<void(PooledConnection::Ptr, bool)>;

    ConnectionGuard(PooledConnection::Ptr conn, ReleaseFunc release_func);
    ~ConnectionGuard();

    // Move-only
    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;
    ConnectionGuard(ConnectionGuard&& other) noexcept;
    ConnectionGuard& operator=(ConnectionGuard&& other) noexcept;

    /**
     * Get the underlying connection
     */
    PooledConnection& operator*() { return *conn_; }
    PooledConnection* operator->() { return conn_.get(); }
    PooledConnection::Ptr get() { return conn_; }

    /**
     * Mark connection as failed (won't be returned to pool)
     */
    void mark_failed() { failed_ = true; }

    /**
     * Release the connection early (returns to pool or discards)
     */
    void release();

    /**
     * Check if guard holds a valid connection
     */
    explicit operator bool() const { return conn_ != nullptr; }

private:
    PooledConnection::Ptr conn_;
    ReleaseFunc release_func_;
    bool failed_{false};
    bool released_{false};
};

/**
 * Connection pool for a single backend
 */
class BackendPool {
public:
    BackendPool(asio::io_context& io_context,
                const config::BackendConfig& backend,
                const ConnectionPoolConfig& config);
    ~BackendPool();

    // Non-copyable
    BackendPool(const BackendPool&) = delete;
    BackendPool& operator=(const BackendPool&) = delete;

    /**
     * Get a connection from the pool (synchronous, blocks until available)
     * Returns nullopt if cannot get connection within timeout
     */
    std::optional<ConnectionGuard> get_connection();

    /**
     * Return a connection to the pool
     * @param conn The connection to return
     * @param reusable Whether the connection can be reused (false if error occurred)
     */
    void return_connection(PooledConnection::Ptr conn, bool reusable);

    /**
     * Clean up idle connections
     */
    void cleanup_idle();

    /**
     * Close all connections and reset pool
     */
    void close_all();

    /**
     * Get current number of available connections in pool
     */
    std::size_t available_count() const;

    /**
     * Get current number of connections in use
     */
    std::size_t in_use_count() const;

    /**
     * Get total connections (available + in use)
     */
    std::size_t total_count() const;

    /**
     * Get backend configuration
     */
    const config::BackendConfig& backend() const { return backend_; }

private:
    /**
     * Create a new connection to the backend
     */
    PooledConnection::Ptr create_connection();

    asio::io_context& io_context_;
    config::BackendConfig backend_;
    ConnectionPoolConfig config_;

    mutable std::mutex mutex_;
    std::deque<PooledConnection::Ptr> available_;  // Available connections
    std::atomic<std::size_t> in_use_{0};           // Number of connections currently in use
    std::atomic<std::size_t> total_created_{0};    // Total connections ever created
};

/**
 * Connection pool manager - manages pools for all backends
 *
 * Features:
 * - Maintains a pool of persistent connections per backend
 * - Thread-safe connection checkout/checkin
 * - Automatic cleanup of idle/stale connections
 * - RAII-based connection lifecycle via ConnectionGuard
 */
class ConnectionPoolManager : public std::enable_shared_from_this<ConnectionPoolManager> {
public:
    using Ptr = std::shared_ptr<ConnectionPoolManager>;

    /**
     * Create a connection pool manager
     * @param io_context Asio io_context for async operations
     * @param config Pool configuration
     */
    ConnectionPoolManager(asio::io_context& io_context,
                          const ConnectionPoolConfig& config = {});
    ~ConnectionPoolManager();

    // Non-copyable
    ConnectionPoolManager(const ConnectionPoolManager&) = delete;
    ConnectionPoolManager& operator=(const ConnectionPoolManager&) = delete;

    /**
     * Set backends to manage pools for
     * @param backends List of backend configurations
     */
    void set_backends(const std::vector<config::BackendConfig>& backends);

    /**
     * Get a connection to a specific backend
     * @param backend The backend to connect to
     * @return Connection guard if successful, nullopt if failed
     */
    std::optional<ConnectionGuard> get_connection(const config::BackendConfig& backend);

    /**
     * Start the cleanup timer for idle connections
     */
    void start_cleanup();

    /**
     * Stop the cleanup timer
     */
    void stop_cleanup();

    /**
     * Get pool statistics for a backend
     */
    struct PoolStats {
        std::size_t available;
        std::size_t in_use;
        std::size_t total;
    };
    std::optional<PoolStats> get_pool_stats(const config::BackendConfig& backend) const;

    /**
     * Get aggregate statistics for all pools
     */
    PoolStats get_total_stats() const;

    /**
     * Get the configuration
     */
    const ConnectionPoolConfig& config() const { return config_; }

private:
    /**
     * Schedule the next cleanup cycle
     */
    void schedule_cleanup();

    /**
     * Perform cleanup on all pools
     */
    void do_cleanup();

    /**
     * Generate unique key for backend
     */
    static std::string backend_key(const config::BackendConfig& backend);

    asio::io_context& io_context_;
    asio::steady_timer cleanup_timer_;
    ConnectionPoolConfig config_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<BackendPool>> pools_;

    std::atomic<bool> running_{false};
};

} // namespace ntonix::proxy

#endif // NTONIX_PROXY_CONNECTION_POOL_HPP
