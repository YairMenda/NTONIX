/**
 * NTONIX - High-Performance AI Inference Gateway
 * Configuration System Implementation
 */

#include "config/config.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <cstdlib>
#else
#include <cstdlib>
#endif

namespace ntonix::config {

// JSON serialization implementations
void to_json(nlohmann::json& j, const BackendConfig& b) {
    j = nlohmann::json{
        {"host", b.host},
        {"port", b.port},
        {"weight", b.weight}
    };
}

void from_json(const nlohmann::json& j, BackendConfig& b) {
    if (j.contains("host")) j.at("host").get_to(b.host);
    if (j.contains("port")) j.at("port").get_to(b.port);
    if (j.contains("weight")) j.at("weight").get_to(b.weight);
}

void to_json(nlohmann::json& j, const ServerSettings& s) {
    j = nlohmann::json{
        {"port", s.port},
        {"ssl_port", s.ssl_port},
        {"threads", s.threads},
        {"bind_address", s.bind_address}
    };
}

void from_json(const nlohmann::json& j, ServerSettings& s) {
    if (j.contains("port")) j.at("port").get_to(s.port);
    if (j.contains("ssl_port")) j.at("ssl_port").get_to(s.ssl_port);
    if (j.contains("threads")) j.at("threads").get_to(s.threads);
    if (j.contains("bind_address")) j.at("bind_address").get_to(s.bind_address);
}

void to_json(nlohmann::json& j, const CacheSettings& c) {
    j = nlohmann::json{
        {"enabled", c.enabled},
        {"max_size_mb", c.max_size_mb},
        {"ttl_seconds", c.ttl_seconds}
    };
}

void from_json(const nlohmann::json& j, CacheSettings& c) {
    if (j.contains("enabled")) j.at("enabled").get_to(c.enabled);
    if (j.contains("max_size_mb")) j.at("max_size_mb").get_to(c.max_size_mb);
    if (j.contains("ttl_seconds")) j.at("ttl_seconds").get_to(c.ttl_seconds);
}

void to_json(nlohmann::json& j, const SslSettings& s) {
    j = nlohmann::json{
        {"cert_file", s.cert_file},
        {"key_file", s.key_file},
        {"enabled", s.enabled}
    };
}

void from_json(const nlohmann::json& j, SslSettings& s) {
    if (j.contains("cert_file")) j.at("cert_file").get_to(s.cert_file);
    if (j.contains("key_file")) j.at("key_file").get_to(s.key_file);
    if (j.contains("enabled")) j.at("enabled").get_to(s.enabled);
}

void to_json(nlohmann::json& j, const LogSettings& l) {
    j = nlohmann::json{
        {"level", l.level},
        {"file", l.file},
        {"max_file_size_mb", l.max_file_size_mb},
        {"max_files", l.max_files},
        {"enable_console", l.enable_console},
        {"enable_colors", l.enable_colors}
    };
}

void from_json(const nlohmann::json& j, LogSettings& l) {
    if (j.contains("level")) j.at("level").get_to(l.level);
    if (j.contains("file")) j.at("file").get_to(l.file);
    if (j.contains("max_file_size_mb")) j.at("max_file_size_mb").get_to(l.max_file_size_mb);
    if (j.contains("max_files")) j.at("max_files").get_to(l.max_files);
    if (j.contains("enable_console")) j.at("enable_console").get_to(l.enable_console);
    if (j.contains("enable_colors")) j.at("enable_colors").get_to(l.enable_colors);
}

void to_json(nlohmann::json& j, const Config& c) {
    j = nlohmann::json{
        {"server", c.server},
        {"backends", c.backends},
        {"cache", c.cache},
        {"ssl", c.ssl},
        {"logging", c.logging}
    };
}

void from_json(const nlohmann::json& j, Config& c) {
    if (j.contains("server")) j.at("server").get_to(c.server);
    if (j.contains("backends")) j.at("backends").get_to(c.backends);
    if (j.contains("cache")) j.at("cache").get_to(c.cache);
    if (j.contains("ssl")) j.at("ssl").get_to(c.ssl);
    if (j.contains("logging")) j.at("logging").get_to(c.logging);
}

// Config validation
void Config::validate() const {
    // Validate server settings
    if (server.port == 0) {
        throw std::runtime_error("Configuration error: server.port must be non-zero");
    }
    if (server.ssl_port == 0) {
        throw std::runtime_error("Configuration error: server.ssl_port must be non-zero");
    }
    if (server.port == server.ssl_port) {
        throw std::runtime_error("Configuration error: server.port and server.ssl_port must be different");
    }
    if (server.bind_address.empty()) {
        throw std::runtime_error("Configuration error: server.bind_address cannot be empty");
    }

    // Validate backends
    for (std::size_t i = 0; i < backends.size(); ++i) {
        const auto& backend = backends[i];
        if (backend.host.empty()) {
            throw std::runtime_error("Configuration error: backends[" + std::to_string(i) + "].host cannot be empty");
        }
        if (backend.port == 0) {
            throw std::runtime_error("Configuration error: backends[" + std::to_string(i) + "].port must be non-zero");
        }
        if (backend.weight == 0) {
            throw std::runtime_error("Configuration error: backends[" + std::to_string(i) + "].weight must be non-zero");
        }
    }

    // Validate cache settings
    if (cache.enabled && cache.max_size_mb == 0) {
        throw std::runtime_error("Configuration error: cache.max_size_mb must be non-zero when cache is enabled");
    }

    // Validate SSL settings
    if (ssl.enabled) {
        if (ssl.cert_file.empty()) {
            throw std::runtime_error("Configuration error: ssl.cert_file cannot be empty when SSL is enabled");
        }
        if (ssl.key_file.empty()) {
            throw std::runtime_error("Configuration error: ssl.key_file cannot be empty when SSL is enabled");
        }
    }

    spdlog::debug("Configuration validated successfully");
}

// ConfigManager implementation
ConfigManager::ConfigManager() = default;
ConfigManager::~ConfigManager() = default;

bool ConfigManager::load(int argc, char* argv[]) {
    std::lock_guard<std::mutex> lock(config_mutex_);

    // Start with defaults
    config_ = Config{};

    // First pass: look for --help or --config
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return false;
        }

        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path_ = argv[++i];
        } else if (arg.starts_with("--config=")) {
            config_path_ = arg.substr(9);
        } else if (arg.starts_with("-c=")) {
            config_path_ = arg.substr(3);
        }
    }

    // Load from config file if specified
    if (!config_path_.empty()) {
        load_from_file(config_path_);
    }

    // Apply environment variable overrides
    apply_environment_overrides();

    // Apply CLI overrides (highest precedence)
    apply_cli_overrides(argc, argv);

    // Validate final configuration
    config_.validate();

    spdlog::info("Configuration loaded successfully");
    return true;
}

Config ConfigManager::get_config() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void ConfigManager::reload() {
    std::lock_guard<std::mutex> lock(config_mutex_);

    if (config_path_.empty()) {
        spdlog::warn("No configuration file specified, reload skipped");
        return;
    }

    spdlog::info("Reloading configuration from {}", config_path_.string());

    try {
        // Store current backends for comparison
        auto old_backends = config_.backends;

        // Reload from file
        load_from_file(config_path_);

        // Re-apply environment overrides (in case they changed)
        apply_environment_overrides();

        // Re-apply CLI overrides (stored, not re-parsed)
        if (cli_port_) config_.server.port = *cli_port_;
        if (cli_ssl_port_) config_.server.ssl_port = *cli_ssl_port_;
        if (cli_threads_) config_.server.threads = *cli_threads_;
        if (cli_bind_address_) config_.server.bind_address = *cli_bind_address_;

        // Validate
        config_.validate();

        // Notify callbacks if backends changed
        if (config_.backends != old_backends) {
            spdlog::info("Backend configuration changed, notifying {} listeners",
                        reload_callbacks_.size());
            for (const auto& callback : reload_callbacks_) {
                callback(config_.backends);
            }
        } else {
            spdlog::info("Configuration reloaded, no backend changes");
        }

    } catch (const std::exception& e) {
        spdlog::error("Configuration reload failed: {}", e.what());
        // Keep existing configuration on error
    }
}

void ConfigManager::on_reload(ConfigReloadCallback callback) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    reload_callbacks_.push_back(std::move(callback));
}

std::filesystem::path ConfigManager::get_config_path() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_path_;
}

void ConfigManager::print_help(const char* program_name) {
    std::cout << "NTONIX - High-Performance AI Inference Gateway\n"
              << "\n"
              << "Usage: " << program_name << " [OPTIONS]\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help              Show this help message and exit\n"
              << "  -c, --config FILE       Path to JSON configuration file\n"
              << "  -p, --port PORT         Server HTTP port (default: 8080)\n"
              << "  --ssl-port PORT         Server HTTPS port (default: 8443)\n"
              << "  -t, --threads NUM       Number of I/O threads (default: CPU cores)\n"
              << "  -b, --bind ADDRESS      Bind address (default: 0.0.0.0)\n"
              << "  --backends HOST:PORT    Backend server (can be repeated)\n"
              << "\n"
              << "Environment Variables:\n"
              << "  NTONIX_PORT             Server HTTP port\n"
              << "  NTONIX_SSL_PORT         Server HTTPS port\n"
              << "  NTONIX_THREADS          Number of I/O threads\n"
              << "  NTONIX_BIND             Bind address\n"
              << "  NTONIX_BACKENDS         Comma-separated backends (host:port,...)\n"
              << "  NTONIX_CONFIG           Path to configuration file\n"
              << "  NTONIX_CACHE_ENABLED    Enable/disable cache (true/false)\n"
              << "  NTONIX_CACHE_SIZE_MB    Cache size in MB\n"
              << "  NTONIX_CACHE_TTL        Cache TTL in seconds\n"
              << "  NTONIX_LOG_LEVEL        Log level (trace/debug/info/warn/error/critical/off)\n"
              << "  NTONIX_LOG_FILE         Log file path (stdout if not set)\n"
              << "\n"
              << "Configuration Precedence (highest to lowest):\n"
              << "  1. Command-line arguments\n"
              << "  2. Environment variables\n"
              << "  3. Configuration file\n"
              << "  4. Default values\n"
              << "\n"
              << "Configuration File Format (JSON):\n"
              << "  {\n"
              << "    \"server\": {\n"
              << "      \"port\": 8080,\n"
              << "      \"ssl_port\": 8443,\n"
              << "      \"threads\": 4\n"
              << "    },\n"
              << "    \"backends\": [\n"
              << "      {\"host\": \"localhost\", \"port\": 8001, \"weight\": 1}\n"
              << "    ],\n"
              << "    \"cache\": {\n"
              << "      \"enabled\": true,\n"
              << "      \"max_size_mb\": 512,\n"
              << "      \"ttl_seconds\": 3600\n"
              << "    },\n"
              << "    \"ssl\": {\n"
              << "      \"enabled\": false,\n"
              << "      \"cert_file\": \"server.crt\",\n"
              << "      \"key_file\": \"server.key\"\n"
              << "    },\n"
              << "    \"logging\": {\n"
              << "      \"level\": \"info\",\n"
              << "      \"file\": \"\",\n"
              << "      \"max_file_size_mb\": 100,\n"
              << "      \"max_files\": 5,\n"
              << "      \"enable_console\": true,\n"
              << "      \"enable_colors\": true\n"
              << "    }\n"
              << "  }\n"
              << "\n"
              << "Send SIGHUP to reload backend configuration without restart.\n";
}

void ConfigManager::load_from_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Configuration file not found: " + path.string());
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open configuration file: " + path.string());
    }

    try {
        nlohmann::json j = nlohmann::json::parse(file);
        config_ = j.get<Config>();
        spdlog::debug("Loaded configuration from {}", path.string());
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("Invalid JSON in configuration file: " + std::string(e.what()));
    }
}

void ConfigManager::apply_environment_overrides() {
    // Check for config file path from environment
    if (config_path_.empty()) {
        if (auto env = get_env("NTONIX_CONFIG")) {
            config_path_ = *env;
            if (!config_path_.empty()) {
                load_from_file(config_path_);
            }
        }
    }

    // Server settings
    if (auto env = get_env("NTONIX_PORT")) {
        try {
            config_.server.port = static_cast<std::uint16_t>(std::stoul(*env));
            spdlog::debug("Applied NTONIX_PORT={}", config_.server.port);
        } catch (...) {
            throw std::runtime_error("Invalid NTONIX_PORT value: " + *env);
        }
    }

    if (auto env = get_env("NTONIX_SSL_PORT")) {
        try {
            config_.server.ssl_port = static_cast<std::uint16_t>(std::stoul(*env));
            spdlog::debug("Applied NTONIX_SSL_PORT={}", config_.server.ssl_port);
        } catch (...) {
            throw std::runtime_error("Invalid NTONIX_SSL_PORT value: " + *env);
        }
    }

    if (auto env = get_env("NTONIX_THREADS")) {
        try {
            config_.server.threads = std::stoul(*env);
            spdlog::debug("Applied NTONIX_THREADS={}", config_.server.threads);
        } catch (...) {
            throw std::runtime_error("Invalid NTONIX_THREADS value: " + *env);
        }
    }

    if (auto env = get_env("NTONIX_BIND")) {
        config_.server.bind_address = *env;
        spdlog::debug("Applied NTONIX_BIND={}", config_.server.bind_address);
    }

    // Backends (comma-separated host:port format)
    if (auto env = get_env("NTONIX_BACKENDS")) {
        config_.backends.clear();
        std::istringstream stream(*env);
        std::string backend_str;

        while (std::getline(stream, backend_str, ',')) {
            // Trim whitespace
            auto start = backend_str.find_first_not_of(" \t");
            auto end = backend_str.find_last_not_of(" \t");
            if (start == std::string::npos) continue;
            backend_str = backend_str.substr(start, end - start + 1);

            // Parse host:port
            auto colon_pos = backend_str.rfind(':');
            if (colon_pos == std::string::npos) {
                throw std::runtime_error("Invalid backend format (expected host:port): " + backend_str);
            }

            BackendConfig backend;
            backend.host = backend_str.substr(0, colon_pos);
            try {
                backend.port = static_cast<std::uint16_t>(std::stoul(backend_str.substr(colon_pos + 1)));
            } catch (...) {
                throw std::runtime_error("Invalid port in backend: " + backend_str);
            }
            backend.weight = 1;
            config_.backends.push_back(backend);
        }
        spdlog::debug("Applied NTONIX_BACKENDS with {} backends", config_.backends.size());
    }

    // Cache settings
    if (auto env = get_env("NTONIX_CACHE_ENABLED")) {
        config_.cache.enabled = (*env == "true" || *env == "1" || *env == "yes");
        spdlog::debug("Applied NTONIX_CACHE_ENABLED={}", config_.cache.enabled);
    }

    if (auto env = get_env("NTONIX_CACHE_SIZE_MB")) {
        try {
            config_.cache.max_size_mb = std::stoul(*env);
            spdlog::debug("Applied NTONIX_CACHE_SIZE_MB={}", config_.cache.max_size_mb);
        } catch (...) {
            throw std::runtime_error("Invalid NTONIX_CACHE_SIZE_MB value: " + *env);
        }
    }

    if (auto env = get_env("NTONIX_CACHE_TTL")) {
        try {
            config_.cache.ttl_seconds = static_cast<std::uint32_t>(std::stoul(*env));
            spdlog::debug("Applied NTONIX_CACHE_TTL={}", config_.cache.ttl_seconds);
        } catch (...) {
            throw std::runtime_error("Invalid NTONIX_CACHE_TTL value: " + *env);
        }
    }

    // Logging settings
    if (auto env = get_env("NTONIX_LOG_LEVEL")) {
        config_.logging.level = *env;
        spdlog::debug("Applied NTONIX_LOG_LEVEL={}", config_.logging.level);
    }

    if (auto env = get_env("NTONIX_LOG_FILE")) {
        config_.logging.file = *env;
        spdlog::debug("Applied NTONIX_LOG_FILE={}", config_.logging.file);
    }
}

void ConfigManager::apply_cli_overrides(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        // Skip already processed args
        if (arg == "--help" || arg == "-h") continue;
        if (arg == "--config" || arg == "-c") { ++i; continue; }
        if (arg.starts_with("--config=") || arg.starts_with("-c=")) continue;

        // Port
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            try {
                cli_port_ = static_cast<std::uint16_t>(std::stoul(argv[++i]));
                config_.server.port = *cli_port_;
            } catch (...) {
                throw std::runtime_error("Invalid --port value: " + std::string(argv[i]));
            }
        } else if (arg.starts_with("--port=")) {
            try {
                cli_port_ = static_cast<std::uint16_t>(std::stoul(arg.substr(7)));
                config_.server.port = *cli_port_;
            } catch (...) {
                throw std::runtime_error("Invalid --port value: " + arg.substr(7));
            }
        }

        // SSL Port
        else if (arg == "--ssl-port" && i + 1 < argc) {
            try {
                cli_ssl_port_ = static_cast<std::uint16_t>(std::stoul(argv[++i]));
                config_.server.ssl_port = *cli_ssl_port_;
            } catch (...) {
                throw std::runtime_error("Invalid --ssl-port value: " + std::string(argv[i]));
            }
        } else if (arg.starts_with("--ssl-port=")) {
            try {
                cli_ssl_port_ = static_cast<std::uint16_t>(std::stoul(arg.substr(11)));
                config_.server.ssl_port = *cli_ssl_port_;
            } catch (...) {
                throw std::runtime_error("Invalid --ssl-port value: " + arg.substr(11));
            }
        }

        // Threads
        else if ((arg == "--threads" || arg == "-t") && i + 1 < argc) {
            try {
                cli_threads_ = std::stoul(argv[++i]);
                config_.server.threads = *cli_threads_;
            } catch (...) {
                throw std::runtime_error("Invalid --threads value: " + std::string(argv[i]));
            }
        } else if (arg.starts_with("--threads=")) {
            try {
                cli_threads_ = std::stoul(arg.substr(10));
                config_.server.threads = *cli_threads_;
            } catch (...) {
                throw std::runtime_error("Invalid --threads value: " + arg.substr(10));
            }
        }

        // Bind address
        else if ((arg == "--bind" || arg == "-b") && i + 1 < argc) {
            cli_bind_address_ = argv[++i];
            config_.server.bind_address = *cli_bind_address_;
        } else if (arg.starts_with("--bind=")) {
            cli_bind_address_ = arg.substr(7);
            config_.server.bind_address = *cli_bind_address_;
        }

        // Backends (can be repeated)
        else if (arg == "--backends" && i + 1 < argc) {
            std::string backend_str = argv[++i];
            auto colon_pos = backend_str.rfind(':');
            if (colon_pos == std::string::npos) {
                throw std::runtime_error("Invalid --backends format (expected host:port): " + backend_str);
            }

            BackendConfig backend;
            backend.host = backend_str.substr(0, colon_pos);
            try {
                backend.port = static_cast<std::uint16_t>(std::stoul(backend_str.substr(colon_pos + 1)));
            } catch (...) {
                throw std::runtime_error("Invalid port in --backends: " + backend_str);
            }
            backend.weight = 1;
            config_.backends.push_back(backend);
        } else if (arg.starts_with("--backends=")) {
            std::string backend_str = arg.substr(11);
            auto colon_pos = backend_str.rfind(':');
            if (colon_pos == std::string::npos) {
                throw std::runtime_error("Invalid --backends format (expected host:port): " + backend_str);
            }

            BackendConfig backend;
            backend.host = backend_str.substr(0, colon_pos);
            try {
                backend.port = static_cast<std::uint16_t>(std::stoul(backend_str.substr(colon_pos + 1)));
            } catch (...) {
                throw std::runtime_error("Invalid port in --backends: " + backend_str);
            }
            backend.weight = 1;
            config_.backends.push_back(backend);
        }

        // Unknown argument (not an error, might be handled elsewhere)
    }
}

std::optional<std::string> ConfigManager::get_env(const std::string& name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name.c_str()) == 0 && value != nullptr) {
        std::string result(value);
        free(value);
        return result;
    }
    return std::nullopt;
#else
    const char* value = std::getenv(name.c_str());
    if (value) {
        return std::string(value);
    }
    return std::nullopt;
#endif
}

} // namespace ntonix::config
