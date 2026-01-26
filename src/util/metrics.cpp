/**
 * NTONIX - High-Performance AI Inference Gateway
 * Metrics Implementation
 */

#include "util/metrics.hpp"

#include <iomanip>
#include <sstream>

namespace ntonix::util {

// Static instance pointer
Metrics* Metrics::instance_ = nullptr;

Metrics::Metrics()
    : start_time_(std::chrono::steady_clock::now())
{
}

Metrics& Metrics::instance() {
    // Thread-safe lazy initialization (C++11 guarantees)
    static Metrics instance;
    return instance;
}

void Metrics::init(const std::vector<config::BackendConfig>& backends) {
    set_backends(backends);
}

void Metrics::set_backends(const std::vector<config::BackendConfig>& backends) {
    std::lock_guard<std::mutex> lock(backends_mutex_);

    // Clear old backends and create new ones
    backends_.clear();

    for (const auto& backend : backends) {
        auto key = backend_key(backend.host, backend.port);
        auto metrics = std::make_shared<BackendMetrics>();
        metrics->host = backend.host;
        metrics->port = backend.port;
        backends_[key] = metrics;
    }
}

std::string Metrics::backend_key(const std::string& host, std::uint16_t port) {
    return host + ":" + std::to_string(port);
}

void Metrics::request_started() {
    requests_total_.fetch_add(1, std::memory_order_relaxed);
    requests_active_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::request_completed(bool success, std::chrono::milliseconds /*latency*/) {
    requests_active_.fetch_sub(1, std::memory_order_relaxed);
    if (success) {
        requests_success_.fetch_add(1, std::memory_order_relaxed);
    } else {
        requests_error_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Metrics::cache_hit() {
    cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::cache_miss() {
    cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::connection_opened() {
    connections_active_.fetch_add(1, std::memory_order_relaxed);
    connections_total_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::connection_closed() {
    connections_active_.fetch_sub(1, std::memory_order_relaxed);
}

void Metrics::backend_request(const std::string& host, std::uint16_t port,
                              bool success, std::chrono::milliseconds latency) {
    auto key = backend_key(host, port);

    std::shared_ptr<BackendMetrics> metrics;
    {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        auto it = backends_.find(key);
        if (it != backends_.end()) {
            metrics = it->second;
        }
    }

    if (metrics) {
        metrics->requests_total.fetch_add(1, std::memory_order_relaxed);
        if (success) {
            metrics->requests_success.fetch_add(1, std::memory_order_relaxed);
        } else {
            metrics->requests_error.fetch_add(1, std::memory_order_relaxed);
        }
        metrics->latency_sum_ms.fetch_add(latency.count(), std::memory_order_relaxed);
        metrics->latency_count.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t Metrics::uptime_seconds() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
}

void Metrics::set_cache_memory(std::uint64_t bytes) {
    cache_memory_bytes_.store(bytes, std::memory_order_relaxed);
}

MetricsSnapshot Metrics::snapshot() const {
    MetricsSnapshot snap;

    // Request metrics
    snap.requests_total = requests_total_.load(std::memory_order_relaxed);
    snap.requests_active = requests_active_.load(std::memory_order_relaxed);
    snap.requests_success = requests_success_.load(std::memory_order_relaxed);
    snap.requests_error = requests_error_.load(std::memory_order_relaxed);

    // Cache metrics
    snap.cache_hits = cache_hits_.load(std::memory_order_relaxed);
    snap.cache_misses = cache_misses_.load(std::memory_order_relaxed);
    auto cache_total = snap.cache_hits + snap.cache_misses;
    snap.cache_hit_rate = cache_total > 0
        ? static_cast<double>(snap.cache_hits) / cache_total
        : 0.0;

    // System metrics
    snap.uptime_seconds = uptime_seconds();
    snap.connections_active = connections_active_.load(std::memory_order_relaxed);
    snap.connections_total = connections_total_.load(std::memory_order_relaxed);
    snap.memory_cache_bytes = cache_memory_bytes_.load(std::memory_order_relaxed);

    // Per-backend metrics
    {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        for (const auto& [key, metrics] : backends_) {
            MetricsSnapshot::BackendSnapshot backend_snap;
            backend_snap.host = metrics->host;
            backend_snap.port = metrics->port;
            backend_snap.requests = metrics->requests_total.load(std::memory_order_relaxed);
            backend_snap.errors = metrics->requests_error.load(std::memory_order_relaxed);
            backend_snap.latency_avg_ms = metrics->latency_avg_ms();
            backend_snap.error_rate = metrics->error_rate();
            snap.backends.push_back(backend_snap);
        }
    }

    return snap;
}

std::string MetricsSnapshot::to_json() const {
    std::ostringstream json;
    json << std::fixed << std::setprecision(4);

    json << "{\n";

    // Request metrics
    json << "  \"requests\": {\n";
    json << "    \"total\": " << requests_total << ",\n";
    json << "    \"active\": " << requests_active << ",\n";
    json << "    \"success\": " << requests_success << ",\n";
    json << "    \"error\": " << requests_error << "\n";
    json << "  },\n";

    // Cache metrics
    json << "  \"cache\": {\n";
    json << "    \"hits\": " << cache_hits << ",\n";
    json << "    \"misses\": " << cache_misses << ",\n";
    json << "    \"hit_rate\": " << cache_hit_rate << "\n";
    json << "  },\n";

    // System metrics
    json << "  \"system\": {\n";
    json << "    \"uptime_seconds\": " << uptime_seconds << ",\n";
    json << "    \"connections_active\": " << connections_active << ",\n";
    json << "    \"connections_total\": " << connections_total << ",\n";
    json << "    \"memory_cache_bytes\": " << memory_cache_bytes << "\n";
    json << "  },\n";

    // Per-backend metrics
    json << "  \"backends\": [\n";
    for (size_t i = 0; i < backends.size(); ++i) {
        const auto& b = backends[i];
        json << "    {\n";
        json << "      \"host\": \"" << b.host << "\",\n";
        json << "      \"port\": " << b.port << ",\n";
        json << "      \"requests\": " << b.requests << ",\n";
        json << "      \"errors\": " << b.errors << ",\n";
        json << "      \"latency_avg_ms\": " << b.latency_avg_ms << ",\n";
        json << "      \"error_rate\": " << b.error_rate << "\n";
        json << "    }";
        if (i < backends.size() - 1) {
            json << ",";
        }
        json << "\n";
    }
    json << "  ]\n";

    json << "}";

    return json.str();
}

} // namespace ntonix::util
