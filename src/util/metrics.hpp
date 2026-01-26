/**
 * NTONIX - High-Performance AI Inference Gateway
 * Metrics - Thread-safe statistics collection for monitoring
 *
 * Provides:
 * - Request counters (total, active, errors)
 * - Cache hit/miss statistics
 * - Per-backend metrics (requests, errors, latency)
 * - System metrics (uptime, connections)
 * - Thread-safe collection using atomics
 */

#ifndef NTONIX_UTIL_METRICS_HPP
#define NTONIX_UTIL_METRICS_HPP

#include "config/config.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ntonix::util {

/**
 * Per-backend metrics
 */
struct BackendMetrics {
    std::string host;
    std::uint16_t port{0};

    std::atomic<std::uint64_t> requests_total{0};
    std::atomic<std::uint64_t> requests_success{0};
    std::atomic<std::uint64_t> requests_error{0};

    // Latency tracking (exponential moving average)
    std::atomic<std::uint64_t> latency_sum_ms{0};
    std::atomic<std::uint64_t> latency_count{0};

    // Computed metrics
    double latency_avg_ms() const {
        auto count = latency_count.load(std::memory_order_relaxed);
        if (count == 0) return 0.0;
        return static_cast<double>(latency_sum_ms.load(std::memory_order_relaxed)) / count;
    }

    double error_rate() const {
        auto total = requests_total.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(requests_error.load(std::memory_order_relaxed)) / total;
    }
};

/**
 * Global metrics snapshot
 */
struct MetricsSnapshot {
    // Request metrics
    std::uint64_t requests_total{0};
    std::uint64_t requests_active{0};
    std::uint64_t requests_success{0};
    std::uint64_t requests_error{0};

    // Cache metrics
    std::uint64_t cache_hits{0};
    std::uint64_t cache_misses{0};
    double cache_hit_rate{0.0};

    // System metrics
    std::uint64_t uptime_seconds{0};
    std::uint64_t connections_active{0};
    std::uint64_t connections_total{0};

    // Memory usage (approximated from cache size)
    std::uint64_t memory_cache_bytes{0};

    // Per-backend metrics
    struct BackendSnapshot {
        std::string host;
        std::uint16_t port{0};
        std::uint64_t requests{0};
        std::uint64_t errors{0};
        double latency_avg_ms{0.0};
        double error_rate{0.0};
    };
    std::vector<BackendSnapshot> backends;

    /**
     * Serialize to JSON string
     */
    std::string to_json() const;
};

/**
 * Metrics collector - centralized statistics tracking
 *
 * Thread-safe using atomics for lock-free counters.
 * Singleton pattern for easy access from anywhere in the application.
 */
class Metrics {
public:
    /**
     * Get the metrics instance (creates on first call)
     */
    static Metrics& instance();

    /**
     * Initialize metrics with backend configuration
     */
    void init(const std::vector<config::BackendConfig>& backends);

    /**
     * Update backends (on config reload)
     */
    void set_backends(const std::vector<config::BackendConfig>& backends);

    // Request tracking
    void request_started();
    void request_completed(bool success, std::chrono::milliseconds latency);

    // Cache tracking
    void cache_hit();
    void cache_miss();

    // Connection tracking
    void connection_opened();
    void connection_closed();

    // Backend-specific tracking
    void backend_request(const std::string& host, std::uint16_t port,
                        bool success, std::chrono::milliseconds latency);

    /**
     * Get a snapshot of current metrics
     */
    MetricsSnapshot snapshot() const;

    /**
     * Get uptime in seconds
     */
    std::uint64_t uptime_seconds() const;

    /**
     * Update memory usage from cache
     */
    void set_cache_memory(std::uint64_t bytes);

private:
    Metrics();
    ~Metrics() = default;

    // Non-copyable
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // Generate backend key
    static std::string backend_key(const std::string& host, std::uint16_t port);

    // Global counters
    std::atomic<std::uint64_t> requests_total_{0};
    std::atomic<std::uint64_t> requests_active_{0};
    std::atomic<std::uint64_t> requests_success_{0};
    std::atomic<std::uint64_t> requests_error_{0};

    std::atomic<std::uint64_t> cache_hits_{0};
    std::atomic<std::uint64_t> cache_misses_{0};

    std::atomic<std::uint64_t> connections_active_{0};
    std::atomic<std::uint64_t> connections_total_{0};

    std::atomic<std::uint64_t> cache_memory_bytes_{0};

    // Per-backend metrics
    mutable std::mutex backends_mutex_;
    std::unordered_map<std::string, std::shared_ptr<BackendMetrics>> backends_;

    // Start time for uptime calculation
    std::chrono::steady_clock::time_point start_time_;

    static Metrics* instance_;
};

} // namespace ntonix::util

#endif // NTONIX_UTIL_METRICS_HPP
