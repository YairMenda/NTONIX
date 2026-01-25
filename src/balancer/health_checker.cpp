/**
 * NTONIX - High-Performance AI Inference Gateway
 * Backend Health Monitoring - Implementation
 */

#include "balancer/health_checker.hpp"

#include <boost/beast/version.hpp>

namespace ntonix::balancer {

HealthChecker::HealthChecker(asio::io_context& io_context, const HealthCheckConfig& config)
    : io_context_(io_context)
    , timer_(io_context)
    , config_(config)
{
    spdlog::debug("HealthChecker created with interval={}ms, timeout={}ms, "
                  "unhealthy_threshold={}, healthy_threshold={}",
                  config_.interval.count(), config_.timeout.count(),
                  config_.unhealthy_threshold, config_.healthy_threshold);
}

HealthChecker::~HealthChecker() {
    stop();
}

void HealthChecker::set_backends(const std::vector<config::BackendConfig>& backends) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Track existing backends to detect removed ones
    std::unordered_map<std::string, BackendHealth> new_backends;

    for (const auto& backend : backends) {
        std::string key = backend_key(backend);

        // Preserve existing health state if backend exists
        auto it = backends_.find(key);
        if (it != backends_.end()) {
            new_backends[key] = it->second;
            new_backends[key].config = backend;  // Update config (weight may have changed)
        } else {
            // New backend - starts healthy
            new_backends[key] = BackendHealth{
                .config = backend,
                .state = BackendState::healthy,
                .consecutive_failures = 0,
                .consecutive_successes = 0,
                .last_check_time = std::chrono::steady_clock::now(),
                .last_response_time = std::chrono::milliseconds{0}
            };
            spdlog::info("Added backend {}:{} (weight={})", backend.host, backend.port, backend.weight);
        }
    }

    // Log removed backends
    for (const auto& [key, health] : backends_) {
        if (new_backends.find(key) == new_backends.end()) {
            spdlog::info("Removed backend {}:{}", health.config.host, health.config.port);
        }
    }

    backends_ = std::move(new_backends);
}

void HealthChecker::start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    spdlog::info("HealthChecker started");
    schedule_health_check();
}

void HealthChecker::stop() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    timer_.cancel();
    spdlog::info("HealthChecker stopped");
}

std::vector<config::BackendConfig> HealthChecker::get_healthy_backends() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<config::BackendConfig> healthy;

    for (const auto& [key, health] : backends_) {
        if (health.state == BackendState::healthy) {
            healthy.push_back(health.config);
        }
    }

    return healthy;
}

std::vector<BackendHealth> HealthChecker::get_all_backends() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BackendHealth> all;

    for (const auto& [key, health] : backends_) {
        all.push_back(health);
    }

    return all;
}

bool HealthChecker::is_healthy(const config::BackendConfig& backend) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(backend_key(backend));
    return it != backends_.end() && it->second.state == BackendState::healthy;
}

void HealthChecker::on_state_change(StateChangeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_callbacks_.push_back(std::move(callback));
}

void HealthChecker::schedule_health_check() {
    if (!running_) {
        return;
    }

    timer_.expires_after(config_.interval);
    timer_.async_wait([self = shared_from_this()](boost::system::error_code ec) {
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                spdlog::warn("Health check timer error: {}", ec.message());
            }
            return;
        }

        // Get backends to check (copy to avoid holding lock during checks)
        std::vector<config::BackendConfig> backends_to_check;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            for (const auto& [key, health] : self->backends_) {
                backends_to_check.push_back(health.config);
            }
        }

        // Check each backend
        for (const auto& backend : backends_to_check) {
            self->check_backend(backend);
        }

        // Schedule next check
        self->schedule_health_check();
    });
}

void HealthChecker::check_backend(const config::BackendConfig& backend) {
    auto start_time = std::chrono::steady_clock::now();

    // Create a new resolver and socket for this check
    auto resolver = std::make_shared<tcp::resolver>(io_context_);
    auto socket = std::make_shared<tcp::socket>(io_context_);

    // Resolve the backend host
    resolver->async_resolve(
        backend.host,
        std::to_string(backend.port),
        [self = shared_from_this(), backend, socket, resolver, start_time]
        (boost::system::error_code ec, tcp::resolver::results_type results) {
            if (ec) {
                spdlog::debug("Health check DNS resolution failed for {}:{}: {}",
                             backend.host, backend.port, ec.message());
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time);
                self->handle_check_result(backend, false, elapsed);
                return;
            }

            // Connect to the backend
            asio::async_connect(
                *socket,
                results,
                [self, backend, socket, start_time]
                (boost::system::error_code ec, const tcp::endpoint&) {
                    if (ec) {
                        spdlog::debug("Health check connect failed for {}:{}: {}",
                                     backend.host, backend.port, ec.message());
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_time);
                        self->handle_check_result(backend, false, elapsed);
                        return;
                    }

                    // Create HTTP request
                    auto req = std::make_shared<http::request<http::empty_body>>(
                        http::verb::get, self->config_.health_path, 11);
                    req->set(http::field::host, backend.host);
                    req->set(http::field::user_agent, "NTONIX-HealthChecker/1.0");
                    req->set(http::field::connection, "close");

                    // Create buffer for response
                    auto buffer = std::make_shared<beast::flat_buffer>();
                    auto res = std::make_shared<http::response<http::string_body>>();

                    // Send request
                    http::async_write(
                        *socket,
                        *req,
                        [self, backend, socket, buffer, res, req, start_time]
                        (boost::system::error_code ec, std::size_t) {
                            if (ec) {
                                spdlog::debug("Health check write failed for {}:{}: {}",
                                             backend.host, backend.port, ec.message());
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start_time);
                                self->handle_check_result(backend, false, elapsed);
                                return;
                            }

                            // Read response
                            http::async_read(
                                *socket,
                                *buffer,
                                *res,
                                [self, backend, socket, buffer, res, start_time]
                                (boost::system::error_code ec, std::size_t) {
                                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - start_time);

                                    if (ec) {
                                        spdlog::debug("Health check read failed for {}:{}: {}",
                                                     backend.host, backend.port, ec.message());
                                        self->handle_check_result(backend, false, elapsed);
                                        return;
                                    }

                                    // Check if response indicates healthy (2xx status)
                                    bool success = (res->result_int() >= 200 && res->result_int() < 300);
                                    spdlog::debug("Health check for {}:{}: status={}, time={}ms",
                                                 backend.host, backend.port,
                                                 res->result_int(), elapsed.count());

                                    self->handle_check_result(backend, success, elapsed);

                                    // Close socket
                                    boost::system::error_code close_ec;
                                    socket->shutdown(tcp::socket::shutdown_both, close_ec);
                                    socket->close(close_ec);
                                }
                            );
                        }
                    );
                }
            );
        }
    );
}

void HealthChecker::handle_check_result(const config::BackendConfig& backend, bool success,
                                        std::chrono::milliseconds response_time) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto key = backend_key(backend);
    auto it = backends_.find(key);
    if (it == backends_.end()) {
        return;  // Backend was removed
    }

    auto& health = it->second;
    health.last_check_time = std::chrono::steady_clock::now();
    health.last_response_time = response_time;

    BackendState old_state = health.state;
    BackendState new_state = old_state;

    if (success) {
        health.consecutive_failures = 0;
        health.consecutive_successes++;

        if (health.state == BackendState::unhealthy &&
            health.consecutive_successes >= config_.healthy_threshold) {
            new_state = BackendState::healthy;
        }
    } else {
        health.consecutive_successes = 0;
        health.consecutive_failures++;

        if (health.state == BackendState::healthy &&
            health.consecutive_failures >= config_.unhealthy_threshold) {
            new_state = BackendState::unhealthy;
        }
    }

    if (new_state != old_state) {
        health.state = new_state;

        // Log state transition
        spdlog::info("Backend {}:{} state changed: {} -> {}",
                     backend.host, backend.port,
                     to_string(old_state), to_string(new_state));

        // Copy callbacks to call outside lock
        auto callbacks = state_callbacks_;

        // Unlock before calling callbacks
        mutex_.unlock();
        for (const auto& callback : callbacks) {
            try {
                callback(backend, old_state, new_state);
            } catch (const std::exception& e) {
                spdlog::error("State change callback error: {}", e.what());
            }
        }
        mutex_.lock();
    }
}

void HealthChecker::update_state(const config::BackendConfig& backend, BackendState new_state) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto key = backend_key(backend);
    auto it = backends_.find(key);
    if (it == backends_.end()) {
        return;
    }

    BackendState old_state = it->second.state;
    if (old_state == new_state) {
        return;
    }

    it->second.state = new_state;

    spdlog::info("Backend {}:{} state changed: {} -> {}",
                 backend.host, backend.port,
                 to_string(old_state), to_string(new_state));

    for (const auto& callback : state_callbacks_) {
        try {
            callback(backend, old_state, new_state);
        } catch (const std::exception& e) {
            spdlog::error("State change callback error: {}", e.what());
        }
    }
}

std::string HealthChecker::backend_key(const config::BackendConfig& backend) {
    return backend.host + ":" + std::to_string(backend.port);
}

} // namespace ntonix::balancer
