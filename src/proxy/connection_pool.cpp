/**
 * NTONIX - High-Performance AI Inference Gateway
 * Backend Connection Pool - Implementation
 */

#include "proxy/connection_pool.hpp"

#include <boost/asio/connect.hpp>
#include <unordered_set>

namespace ntonix::proxy {

// ============================================================================
// PooledConnection Implementation
// ============================================================================

PooledConnection::PooledConnection(tcp::socket socket, const config::BackendConfig& backend)
    : socket_(std::move(socket))
    , backend_(backend)
    , last_used_(std::chrono::steady_clock::now())
    , usage_count_(0)
    , in_use_(false) {
}

PooledConnection::~PooledConnection() {
    if (socket_.is_open()) {
        boost::system::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

bool PooledConnection::is_valid() const {
    return socket_.is_open();
}

bool PooledConnection::is_idle(std::chrono::seconds max_idle) const {
    if (in_use_) return false;
    auto now = std::chrono::steady_clock::now();
    return (now - last_used_) > max_idle;
}

void PooledConnection::mark_in_use() {
    in_use_ = true;
    usage_count_++;
    last_used_ = std::chrono::steady_clock::now();
}

void PooledConnection::mark_returned() {
    in_use_ = false;
    last_used_ = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::duration PooledConnection::idle_time() const {
    if (in_use_) return std::chrono::steady_clock::duration::zero();
    return std::chrono::steady_clock::now() - last_used_;
}

// ============================================================================
// ConnectionGuard Implementation
// ============================================================================

ConnectionGuard::ConnectionGuard(PooledConnection::Ptr conn, ReleaseFunc release_func)
    : conn_(std::move(conn))
    , release_func_(std::move(release_func))
    , failed_(false)
    , released_(false) {
}

ConnectionGuard::~ConnectionGuard() {
    release();
}

ConnectionGuard::ConnectionGuard(ConnectionGuard&& other) noexcept
    : conn_(std::move(other.conn_))
    , release_func_(std::move(other.release_func_))
    , failed_(other.failed_)
    , released_(other.released_) {
    other.released_ = true;  // Prevent other from releasing
}

ConnectionGuard& ConnectionGuard::operator=(ConnectionGuard&& other) noexcept {
    if (this != &other) {
        release();  // Release our current connection first
        conn_ = std::move(other.conn_);
        release_func_ = std::move(other.release_func_);
        failed_ = other.failed_;
        released_ = other.released_;
        other.released_ = true;
    }
    return *this;
}

void ConnectionGuard::release() {
    if (!released_ && conn_ && release_func_) {
        released_ = true;
        release_func_(std::move(conn_), !failed_);
    }
}

// ============================================================================
// BackendPool Implementation
// ============================================================================

BackendPool::BackendPool(asio::io_context& io_context,
                         const config::BackendConfig& backend,
                         const ConnectionPoolConfig& config)
    : io_context_(io_context)
    , backend_(backend)
    , config_(config)
    , in_use_(0)
    , total_created_(0) {
}

BackendPool::~BackendPool() {
    close_all();
}

std::optional<ConnectionGuard> BackendPool::get_connection() {
    PooledConnection::Ptr conn;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Try to get an existing connection from the pool
        while (!available_.empty()) {
            auto candidate = available_.front();
            available_.pop_front();

            if (candidate->is_valid()) {
                conn = candidate;
                break;
            }
            // Invalid connection, discard it
            spdlog::debug("Discarding invalid pooled connection to {}:{}",
                         backend_.host, backend_.port);
        }
    }

    // If no available connection, create a new one if under limit
    if (!conn) {
        std::size_t current_total = available_count() + in_use_.load();
        if (current_total < config_.pool_size_per_backend) {
            conn = create_connection();
        }
    }

    if (!conn) {
        spdlog::warn("Connection pool exhausted for {}:{} (max={})",
                    backend_.host, backend_.port, config_.pool_size_per_backend);
        return std::nullopt;
    }

    conn->mark_in_use();
    in_use_++;

    // Create release function that returns to this pool
    auto release_func = [this](PooledConnection::Ptr c, bool reusable) {
        return_connection(std::move(c), reusable);
    };

    return ConnectionGuard(std::move(conn), std::move(release_func));
}

void BackendPool::return_connection(PooledConnection::Ptr conn, bool reusable) {
    if (in_use_ > 0) {
        in_use_--;
    }

    if (!conn) return;

    conn->mark_returned();

    if (reusable && conn->is_valid()) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Add back to the front (LIFO) for better cache locality
        available_.push_front(std::move(conn));
        spdlog::debug("Returned connection to pool for {}:{} (available={}, in_use={})",
                     backend_.host, backend_.port, available_.size(), in_use_.load());
    } else {
        spdlog::debug("Discarding non-reusable connection to {}:{}",
                     backend_.host, backend_.port);
    }
}

void BackendPool::cleanup_idle() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t removed = 0;
    auto it = available_.begin();
    while (it != available_.end()) {
        if ((*it)->is_idle(config_.idle_timeout) || !(*it)->is_valid()) {
            it = available_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }

    if (removed > 0) {
        spdlog::debug("Cleaned up {} idle connections for {}:{}",
                     removed, backend_.host, backend_.port);
    }
}

void BackendPool::close_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    available_.clear();
    spdlog::debug("Closed all pooled connections for {}:{}", backend_.host, backend_.port);
}

std::size_t BackendPool::available_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_.size();
}

std::size_t BackendPool::in_use_count() const {
    return in_use_.load();
}

std::size_t BackendPool::total_count() const {
    return available_count() + in_use_.load();
}

PooledConnection::Ptr BackendPool::create_connection() {
    try {
        tcp::socket socket(io_context_);

        // Resolve the backend address
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(backend_.host, std::to_string(backend_.port));

        // Connect with timeout
        boost::system::error_code ec;
        asio::connect(socket, endpoints, ec);

        if (ec) {
            spdlog::warn("Failed to connect to backend {}:{}: {}",
                        backend_.host, backend_.port, ec.message());
            return nullptr;
        }

        // Configure socket options
        socket.set_option(tcp::no_delay(true), ec);  // Disable Nagle's algorithm

        if (config_.enable_keep_alive) {
            socket.set_option(asio::socket_base::keep_alive(true), ec);
        }

        total_created_++;
        spdlog::debug("Created new connection to {}:{} (total_created={})",
                     backend_.host, backend_.port, total_created_.load());

        return std::make_shared<PooledConnection>(std::move(socket), backend_);

    } catch (const std::exception& e) {
        spdlog::error("Exception creating connection to {}:{}: {}",
                     backend_.host, backend_.port, e.what());
        return nullptr;
    }
}

// ============================================================================
// ConnectionPoolManager Implementation
// ============================================================================

ConnectionPoolManager::ConnectionPoolManager(asio::io_context& io_context,
                                             const ConnectionPoolConfig& config)
    : io_context_(io_context)
    , cleanup_timer_(io_context)
    , config_(config)
    , running_(false) {
}

ConnectionPoolManager::~ConnectionPoolManager() {
    stop_cleanup();
}

void ConnectionPoolManager::set_backends(const std::vector<config::BackendConfig>& backends) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Create a set of new backend keys
    std::unordered_set<std::string> new_keys;
    for (const auto& backend : backends) {
        new_keys.insert(backend_key(backend));
    }

    // Remove pools for backends that no longer exist
    for (auto it = pools_.begin(); it != pools_.end(); ) {
        if (new_keys.find(it->first) == new_keys.end()) {
            spdlog::info("Removing connection pool for backend {}", it->first);
            it = pools_.erase(it);
        } else {
            ++it;
        }
    }

    // Add pools for new backends
    for (const auto& backend : backends) {
        std::string key = backend_key(backend);
        if (pools_.find(key) == pools_.end()) {
            spdlog::info("Creating connection pool for backend {}:{}", backend.host, backend.port);
            pools_[key] = std::make_unique<BackendPool>(io_context_, backend, config_);
        }
    }
}

std::optional<ConnectionGuard> ConnectionPoolManager::get_connection(
    const config::BackendConfig& backend) {
    std::string key = backend_key(backend);

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pools_.find(key);
    if (it == pools_.end()) {
        spdlog::warn("No connection pool for backend {}:{}", backend.host, backend.port);
        return std::nullopt;
    }

    return it->second->get_connection();
}

void ConnectionPoolManager::start_cleanup() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    spdlog::info("Starting connection pool cleanup timer (interval={}s)",
                config_.cleanup_interval.count());
    schedule_cleanup();
}

void ConnectionPoolManager::stop_cleanup() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    cleanup_timer_.cancel();
    spdlog::info("Stopped connection pool cleanup timer");
}

void ConnectionPoolManager::schedule_cleanup() {
    if (!running_) return;

    auto self = shared_from_this();
    cleanup_timer_.expires_after(config_.cleanup_interval);
    cleanup_timer_.async_wait([self](const boost::system::error_code& ec) {
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                spdlog::error("Cleanup timer error: {}", ec.message());
            }
            return;
        }
        self->do_cleanup();
        self->schedule_cleanup();
    });
}

void ConnectionPoolManager::do_cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [key, pool] : pools_) {
        pool->cleanup_idle();
    }
}

std::optional<ConnectionPoolManager::PoolStats> ConnectionPoolManager::get_pool_stats(
    const config::BackendConfig& backend) const {
    std::string key = backend_key(backend);

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pools_.find(key);
    if (it == pools_.end()) {
        return std::nullopt;
    }

    return PoolStats{
        .available = it->second->available_count(),
        .in_use = it->second->in_use_count(),
        .total = it->second->total_count()
    };
}

ConnectionPoolManager::PoolStats ConnectionPoolManager::get_total_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    PoolStats total{0, 0, 0};
    for (const auto& [key, pool] : pools_) {
        total.available += pool->available_count();
        total.in_use += pool->in_use_count();
        total.total += pool->total_count();
    }
    return total;
}

std::string ConnectionPoolManager::backend_key(const config::BackendConfig& backend) {
    return backend.host + ":" + std::to_string(backend.port);
}

} // namespace ntonix::proxy
