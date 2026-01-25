/**
 * NTONIX - High-Performance AI Inference Gateway
 * Load Balancer - Weighted round-robin distribution across backends
 */

#ifndef NTONIX_BALANCER_LOAD_BALANCER_HPP
#define NTONIX_BALANCER_LOAD_BALANCER_HPP

#include "config/config.hpp"
#include "balancer/health_checker.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ntonix::balancer {

/**
 * Result of backend selection
 */
struct BackendSelection {
    config::BackendConfig backend;
    std::size_t index;  // Index in the backend list for debugging/logging
};

/**
 * Weighted Round-Robin Load Balancer
 *
 * Features:
 * - Thread-safe backend selection using atomics
 * - Weighted round-robin algorithm (backends with higher weights get more requests)
 * - Integrates with HealthChecker to skip unhealthy backends
 * - Returns nullopt when no healthy backends are available
 *
 * Algorithm:
 * Uses the Smooth Weighted Round-Robin (SWRR) algorithm for even distribution:
 * 1. Each backend has an effective weight (current_weight) that changes each round
 * 2. On each selection, pick backend with highest current_weight
 * 3. Decrease selected backend's weight by total_weight
 * 4. Increase all backends' current_weight by their configured weight
 *
 * This ensures backends with weight [5, 1, 1] get selected in pattern like:
 * A, A, B, A, A, C, A (distributed, not A,A,A,A,A,B,C)
 */
class LoadBalancer {
public:
    /**
     * Create a load balancer
     * @param health_checker Optional health checker for filtering unhealthy backends
     */
    explicit LoadBalancer(std::shared_ptr<HealthChecker> health_checker = nullptr);
    ~LoadBalancer();

    // Non-copyable
    LoadBalancer(const LoadBalancer&) = delete;
    LoadBalancer& operator=(const LoadBalancer&) = delete;

    /**
     * Set backends for load balancing
     * @param backends List of backend configurations with weights
     */
    void set_backends(const std::vector<config::BackendConfig>& backends);

    /**
     * Select the next backend using weighted round-robin
     * @return Backend selection if healthy backend available, nullopt otherwise
     *
     * Thread-safe: can be called from multiple threads concurrently
     */
    std::optional<BackendSelection> select_backend();

    /**
     * Get number of configured backends
     */
    std::size_t backend_count() const;

    /**
     * Get number of healthy backends
     */
    std::size_t healthy_backend_count() const;

    /**
     * Check if any healthy backends are available
     */
    bool has_healthy_backends() const;

    /**
     * Get total weight of all backends
     */
    std::uint32_t total_weight() const;

    /**
     * Get total weight of healthy backends only
     */
    std::uint32_t healthy_total_weight() const;

private:
    /**
     * Backend state for weighted round-robin
     */
    struct BackendState {
        config::BackendConfig config;
        std::atomic<std::int64_t> current_weight{0};  // Mutable weight for SWRR
    };

    /**
     * Calculate total weight of all backends
     */
    std::uint32_t calculate_total_weight() const;

    std::shared_ptr<HealthChecker> health_checker_;

    mutable std::mutex mutex_;  // Protects backends_ vector
    std::vector<std::shared_ptr<BackendState>> backends_;
    std::atomic<std::uint32_t> total_weight_{0};
};

} // namespace ntonix::balancer

#endif // NTONIX_BALANCER_LOAD_BALANCER_HPP
