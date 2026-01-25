/**
 * NTONIX - High-Performance AI Inference Gateway
 * Load Balancer - Implementation of weighted round-robin algorithm
 */

#include "balancer/load_balancer.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>

namespace ntonix::balancer {

LoadBalancer::LoadBalancer(std::shared_ptr<HealthChecker> health_checker)
    : health_checker_(std::move(health_checker)) {
    spdlog::debug("LoadBalancer created");
}

LoadBalancer::~LoadBalancer() {
    spdlog::debug("LoadBalancer destroyed");
}

void LoadBalancer::set_backends(const std::vector<config::BackendConfig>& backends) {
    std::lock_guard lock(mutex_);

    // Clear and rebuild backend list
    backends_.clear();
    backends_.reserve(backends.size());

    for (const auto& config : backends) {
        auto state = std::make_shared<BackendState>();
        state->config = config;
        state->current_weight.store(0, std::memory_order_relaxed);
        backends_.push_back(std::move(state));
    }

    // Calculate and store total weight
    total_weight_.store(calculate_total_weight(), std::memory_order_release);

    spdlog::info("LoadBalancer configured with {} backends, total_weight={}",
                 backends_.size(), total_weight_.load(std::memory_order_relaxed));
}

std::optional<BackendSelection> LoadBalancer::select_backend() {
    // Take a snapshot of backends under lock
    std::vector<std::shared_ptr<BackendState>> backends_snapshot;
    {
        std::lock_guard lock(mutex_);
        backends_snapshot = backends_;
    }

    if (backends_snapshot.empty()) {
        spdlog::warn("LoadBalancer: No backends configured");
        return std::nullopt;
    }

    // Smooth Weighted Round-Robin (SWRR) algorithm
    // This is lock-free using atomics for the selection phase

    // Calculate total weight of healthy backends
    std::int64_t healthy_total = 0;
    for (const auto& backend : backends_snapshot) {
        // Check if backend is healthy (if health checker is available)
        if (health_checker_ && !health_checker_->is_healthy(backend->config)) {
            continue;
        }
        healthy_total += backend->config.weight;
    }

    if (healthy_total == 0) {
        spdlog::warn("LoadBalancer: No healthy backends available");
        return std::nullopt;
    }

    // Find backend with highest current_weight among healthy backends
    std::shared_ptr<BackendState> selected = nullptr;
    std::int64_t max_weight = std::numeric_limits<std::int64_t>::min();
    std::size_t selected_index = 0;

    for (std::size_t i = 0; i < backends_snapshot.size(); ++i) {
        auto& backend = backends_snapshot[i];

        // Skip unhealthy backends
        if (health_checker_ && !health_checker_->is_healthy(backend->config)) {
            continue;
        }

        // Atomically add this backend's weight to its current_weight
        // This ensures fair distribution even under concurrent access
        std::int64_t new_weight = backend->current_weight.fetch_add(
            backend->config.weight, std::memory_order_acq_rel) + backend->config.weight;

        if (new_weight > max_weight) {
            max_weight = new_weight;
            selected = backend;
            selected_index = i;
        }
    }

    if (!selected) {
        spdlog::warn("LoadBalancer: Failed to select backend");
        return std::nullopt;
    }

    // Decrease selected backend's weight by total healthy weight
    selected->current_weight.fetch_sub(healthy_total, std::memory_order_release);

    spdlog::debug("LoadBalancer: Selected backend {}:{} (index={}, weight={})",
                  selected->config.host, selected->config.port,
                  selected_index, selected->config.weight);

    return BackendSelection{
        .backend = selected->config,
        .index = selected_index
    };
}

std::size_t LoadBalancer::backend_count() const {
    std::lock_guard lock(mutex_);
    return backends_.size();
}

std::size_t LoadBalancer::healthy_backend_count() const {
    std::lock_guard lock(mutex_);

    if (!health_checker_) {
        // Without health checker, assume all backends are healthy
        return backends_.size();
    }

    std::size_t count = 0;
    for (const auto& backend : backends_) {
        if (health_checker_->is_healthy(backend->config)) {
            ++count;
        }
    }
    return count;
}

bool LoadBalancer::has_healthy_backends() const {
    return healthy_backend_count() > 0;
}

std::uint32_t LoadBalancer::total_weight() const {
    return total_weight_.load(std::memory_order_acquire);
}

std::uint32_t LoadBalancer::healthy_total_weight() const {
    std::lock_guard lock(mutex_);

    if (!health_checker_) {
        // Without health checker, return total weight
        return total_weight_.load(std::memory_order_relaxed);
    }

    std::uint32_t weight = 0;
    for (const auto& backend : backends_) {
        if (health_checker_->is_healthy(backend->config)) {
            weight += backend->config.weight;
        }
    }
    return weight;
}

std::uint32_t LoadBalancer::calculate_total_weight() const {
    std::uint32_t weight = 0;
    for (const auto& backend : backends_) {
        weight += backend->config.weight;
    }
    return weight;
}

} // namespace ntonix::balancer
