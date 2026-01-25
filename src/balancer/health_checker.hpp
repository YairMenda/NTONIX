/**
 * NTONIX - High-Performance AI Inference Gateway
 * Backend Health Monitoring - Periodic health checks with circuit breaker pattern
 */

#ifndef NTONIX_BALANCER_HEALTH_CHECKER_HPP
#define NTONIX_BALANCER_HEALTH_CHECKER_HPP

#include "config/config.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ntonix::balancer {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

/**
 * Backend health state
 */
enum class BackendState {
    healthy,    // Backend is responding to health checks
    unhealthy,  // Backend has failed consecutive health checks
    draining    // Backend is being removed (finish existing requests, no new ones)
};

/**
 * Convert BackendState to string for logging
 */
inline std::string to_string(BackendState state) {
    switch (state) {
        case BackendState::healthy: return "healthy";
        case BackendState::unhealthy: return "unhealthy";
        case BackendState::draining: return "draining";
        default: return "unknown";
    }
}

/**
 * Health check configuration
 */
struct HealthCheckConfig {
    std::chrono::milliseconds interval{5000};       // Check interval (default 5s)
    std::chrono::milliseconds timeout{2000};        // Request timeout (default 2s)
    std::uint32_t unhealthy_threshold{3};           // Failures before marking unhealthy
    std::uint32_t healthy_threshold{2};             // Successes before marking healthy
    std::string health_path{"/health"};             // Health check endpoint
};

/**
 * Backend health status tracking
 */
struct BackendHealth {
    config::BackendConfig config;
    BackendState state{BackendState::healthy};
    std::uint32_t consecutive_failures{0};
    std::uint32_t consecutive_successes{0};
    std::chrono::steady_clock::time_point last_check_time;
    std::chrono::milliseconds last_response_time{0};
};

/**
 * Callback type for state change notifications
 */
using StateChangeCallback = std::function<void(
    const config::BackendConfig& backend,
    BackendState old_state,
    BackendState new_state
)>;

/**
 * Health checker - monitors backend health with circuit breaker pattern
 *
 * Features:
 * - Periodic health check pings to each backend
 * - Circuit breaker: marks unhealthy after N consecutive failures
 * - Automatic recovery when health checks pass again
 * - Thread-safe state access
 * - Logs all state transitions
 */
class HealthChecker : public std::enable_shared_from_this<HealthChecker> {
public:
    /**
     * Create a health checker
     * @param io_context Asio io_context for async operations
     * @param config Health check configuration
     */
    HealthChecker(asio::io_context& io_context, const HealthCheckConfig& config = {});
    ~HealthChecker();

    // Non-copyable
    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;

    /**
     * Set backends to monitor
     * @param backends List of backend configurations
     */
    void set_backends(const std::vector<config::BackendConfig>& backends);

    /**
     * Start health checking
     */
    void start();

    /**
     * Stop health checking
     */
    void stop();

    /**
     * Get list of healthy backends (thread-safe)
     */
    std::vector<config::BackendConfig> get_healthy_backends() const;

    /**
     * Get all backends with their health status (thread-safe)
     */
    std::vector<BackendHealth> get_all_backends() const;

    /**
     * Check if a specific backend is healthy (thread-safe)
     */
    bool is_healthy(const config::BackendConfig& backend) const;

    /**
     * Register callback for state change notifications
     */
    void on_state_change(StateChangeCallback callback);

    /**
     * Get health check configuration
     */
    const HealthCheckConfig& get_config() const noexcept { return config_; }

private:
    /**
     * Schedule the next health check cycle
     */
    void schedule_health_check();

    /**
     * Perform health check on a specific backend
     */
    void check_backend(const config::BackendConfig& backend);

    /**
     * Handle health check result
     */
    void handle_check_result(const config::BackendConfig& backend, bool success,
                            std::chrono::milliseconds response_time);

    /**
     * Update backend state and notify callbacks if changed
     */
    void update_state(const config::BackendConfig& backend, BackendState new_state);

    /**
     * Generate unique key for backend
     */
    static std::string backend_key(const config::BackendConfig& backend);

    asio::io_context& io_context_;
    asio::steady_timer timer_;
    HealthCheckConfig config_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, BackendHealth> backends_;
    std::vector<StateChangeCallback> state_callbacks_;

    std::atomic<bool> running_{false};
};

} // namespace ntonix::balancer

#endif // NTONIX_BALANCER_HEALTH_CHECKER_HPP
