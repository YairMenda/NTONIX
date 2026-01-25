/**
 * NTONIX - High-Performance AI Inference Gateway
 * Configuration System - Supports JSON file, environment variables, and CLI args
 *
 * Configuration hierarchy (highest precedence first):
 * 1. Command-line arguments
 * 2. Environment variables (NTONIX_*)
 * 3. Configuration file (JSON)
 * 4. Default values
 */

#ifndef NTONIX_CONFIG_CONFIG_HPP
#define NTONIX_CONFIG_CONFIG_HPP

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ntonix::config {

/**
 * Backend server configuration
 */
struct BackendConfig {
    std::string host{"localhost"};
    std::uint16_t port{8001};
    std::uint32_t weight{1};

    bool operator==(const BackendConfig&) const = default;
};

/**
 * Server configuration
 */
struct ServerSettings {
    std::uint16_t port{8080};
    std::uint16_t ssl_port{8443};
    std::size_t threads{0};  // 0 = hardware_concurrency
    std::string bind_address{"0.0.0.0"};
};

/**
 * Cache configuration
 */
struct CacheSettings {
    bool enabled{true};
    std::size_t max_size_mb{512};
    std::uint32_t ttl_seconds{3600};
};

/**
 * SSL/TLS configuration
 */
struct SslSettings {
    std::string cert_file{"server.crt"};
    std::string key_file{"server.key"};
    bool enabled{false};
};

/**
 * Complete application configuration
 */
struct Config {
    ServerSettings server;
    std::vector<BackendConfig> backends;
    CacheSettings cache;
    SslSettings ssl;

    /**
     * Validate configuration and throw if invalid
     */
    void validate() const;
};

/**
 * Configuration reload callback type
 */
using ConfigReloadCallback = std::function<void(const std::vector<BackendConfig>&)>;

/**
 * Configuration manager - handles loading, parsing, and hot-reload
 */
class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    // Non-copyable
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    /**
     * Parse command-line arguments and load configuration
     *
     * @param argc Argument count
     * @param argv Argument values
     * @return true if configuration loaded successfully, false if --help was requested
     * @throws std::runtime_error on configuration errors
     */
    bool load(int argc, char* argv[]);

    /**
     * Get the current configuration (thread-safe)
     */
    Config get_config() const;

    /**
     * Reload configuration from file (called on SIGHUP)
     * Only reloads backend list; other settings require restart.
     */
    void reload();

    /**
     * Register callback for configuration reload events
     */
    void on_reload(ConfigReloadCallback callback);

    /**
     * Get the configuration file path
     */
    std::filesystem::path get_config_path() const;

    /**
     * Print help message to stdout
     */
    static void print_help(const char* program_name);

private:
    /**
     * Load configuration from JSON file
     */
    void load_from_file(const std::filesystem::path& path);

    /**
     * Apply environment variable overrides
     */
    void apply_environment_overrides();

    /**
     * Apply command-line argument overrides
     */
    void apply_cli_overrides(int argc, char* argv[]);

    /**
     * Get environment variable value
     */
    static std::optional<std::string> get_env(const std::string& name);

    mutable std::mutex config_mutex_;
    Config config_;
    std::filesystem::path config_path_;
    std::vector<ConfigReloadCallback> reload_callbacks_;

    // CLI overrides (stored to preserve precedence on reload)
    std::optional<std::uint16_t> cli_port_;
    std::optional<std::uint16_t> cli_ssl_port_;
    std::optional<std::size_t> cli_threads_;
    std::optional<std::string> cli_bind_address_;
};

// JSON serialization support
void to_json(nlohmann::json& j, const BackendConfig& b);
void from_json(const nlohmann::json& j, BackendConfig& b);
void to_json(nlohmann::json& j, const ServerSettings& s);
void from_json(const nlohmann::json& j, ServerSettings& s);
void to_json(nlohmann::json& j, const CacheSettings& c);
void from_json(const nlohmann::json& j, CacheSettings& c);
void to_json(nlohmann::json& j, const SslSettings& s);
void from_json(const nlohmann::json& j, SslSettings& s);
void to_json(nlohmann::json& j, const Config& c);
void from_json(const nlohmann::json& j, Config& c);

} // namespace ntonix::config

#endif // NTONIX_CONFIG_CONFIG_HPP
