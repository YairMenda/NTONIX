/**
 * NTONIX - High-Performance AI Inference Gateway
 * Logger - Structured logging with spdlog
 *
 * Provides:
 * - Structured logging with levels (DEBUG, INFO, WARN, ERROR)
 * - Log format: timestamp, level, component, message, context
 * - Access log: method, path, status, latency, cache hit/miss
 * - Configurable log level via config/environment
 * - Log rotation support (or stdout for container deployment)
 * - Request tracing with X-Request-ID propagation
 */

#ifndef NTONIX_UTIL_LOGGER_HPP
#define NTONIX_UTIL_LOGGER_HPP

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace ntonix::util {

/**
 * Log level enumeration
 */
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off
};

/**
 * Logging configuration
 */
struct LogConfig {
    LogLevel level{LogLevel::Info};
    std::string file_path;             // Empty for stdout only
    std::size_t max_file_size_mb{100}; // Max size before rotation
    std::size_t max_files{5};          // Number of rotated files to keep
    bool enable_console{true};         // Log to stdout
    bool enable_colors{true};          // Colored console output
};

/**
 * Access log entry for HTTP requests
 */
struct AccessLogEntry {
    std::string request_id;
    std::string client_ip;
    std::string method;
    std::string path;
    int status_code{0};
    std::size_t request_size{0};
    std::size_t response_size{0};
    std::chrono::milliseconds latency{0};
    bool cache_hit{false};
    std::string backend_host;
    std::uint16_t backend_port{0};
};

/**
 * Logger class - centralized logging with component tagging
 *
 * Thread-safe singleton that manages application-wide logging.
 *
 * Note: Destructor is public to allow std::unique_ptr to clean up the singleton.
 * The singleton pattern is maintained by keeping the constructor private.
 */
class Logger {
public:
    /**
     * Initialize the logger with configuration
     * Must be called before any logging occurs
     */
    static void init(const LogConfig& config);

    /**
     * Initialize with default configuration (stdout, INFO level)
     */
    static void init_default();

    /**
     * Get the logger instance (creates default if not initialized)
     */
    static Logger& instance();

    /**
     * Destructor - public to allow unique_ptr cleanup, but singleton pattern
     * is maintained by keeping constructor private.
     */
    ~Logger();

    /**
     * Set the global log level
     */
    void set_level(LogLevel level);

    /**
     * Get the current log level
     */
    LogLevel get_level() const;

    /**
     * Parse log level from string (case-insensitive)
     * Valid values: trace, debug, info, warn, error, critical, off
     */
    static std::optional<LogLevel> parse_level(std::string_view level_str);

    /**
     * Convert log level to string
     */
    static std::string_view level_to_string(LogLevel level);

    // Component-tagged logging methods
    template<typename... Args>
    void trace(std::string_view component, spdlog::format_string_t<Args...> fmt, Args&&... args) {
        log(LogLevel::Trace, component, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(std::string_view component, spdlog::format_string_t<Args...> fmt, Args&&... args) {
        log(LogLevel::Debug, component, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(std::string_view component, spdlog::format_string_t<Args...> fmt, Args&&... args) {
        log(LogLevel::Info, component, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(std::string_view component, spdlog::format_string_t<Args...> fmt, Args&&... args) {
        log(LogLevel::Warn, component, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(std::string_view component, spdlog::format_string_t<Args...> fmt, Args&&... args) {
        log(LogLevel::Error, component, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void critical(std::string_view component, spdlog::format_string_t<Args...> fmt, Args&&... args) {
        log(LogLevel::Critical, component, fmt, std::forward<Args>(args)...);
    }

    /**
     * Log an HTTP access entry (dedicated access log format)
     */
    void access(const AccessLogEntry& entry);

    /**
     * Shutdown and flush all logs
     */
    void shutdown();

private:
    Logger() = default;

    // Non-copyable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void configure(const LogConfig& config);

    template<typename... Args>
    void log(LogLevel level, std::string_view component, spdlog::format_string_t<Args...> fmt, Args&&... args) {
        if (!logger_) return;

        // Format message with component prefix
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        auto full_msg = fmt::format("[{}] {}", component, msg);

        switch (level) {
            case LogLevel::Trace:    logger_->trace(full_msg); break;
            case LogLevel::Debug:    logger_->debug(full_msg); break;
            case LogLevel::Info:     logger_->info(full_msg); break;
            case LogLevel::Warn:     logger_->warn(full_msg); break;
            case LogLevel::Error:    logger_->error(full_msg); break;
            case LogLevel::Critical: logger_->critical(full_msg); break;
            default: break;
        }
    }

    static spdlog::level::level_enum to_spdlog_level(LogLevel level);
    static LogLevel from_spdlog_level(spdlog::level::level_enum level);

    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<spdlog::logger> access_logger_;
    std::atomic<LogLevel> current_level_{LogLevel::Info};
    mutable std::mutex mutex_;

    static std::unique_ptr<Logger> instance_;
    static std::once_flag init_flag_;
};

/**
 * Request context for X-Request-ID propagation
 *
 * Thread-local storage for request-scoped context.
 * Use RAII-style RequestContext to automatically manage scope.
 */
class RequestContext {
public:
    /**
     * Create a new request context (generates ID if not provided)
     */
    explicit RequestContext(std::string request_id = "");

    /**
     * Destructor clears the thread-local context
     */
    ~RequestContext();

    // Non-copyable, movable
    RequestContext(const RequestContext&) = delete;
    RequestContext& operator=(const RequestContext&) = delete;
    RequestContext(RequestContext&& other) noexcept;
    RequestContext& operator=(RequestContext&& other) noexcept;

    /**
     * Get the current request ID
     */
    const std::string& id() const { return request_id_; }

    /**
     * Get the current thread's request ID (empty if no context)
     */
    static std::string current_id();

    /**
     * Generate a unique request ID
     */
    static std::string generate_id();

private:
    std::string request_id_;
    bool owns_context_{true};
};

// Convenience macros for logging with automatic component tagging
#define NTONIX_LOG_TRACE(component, ...) \
    ::ntonix::util::Logger::instance().trace(component, __VA_ARGS__)
#define NTONIX_LOG_DEBUG(component, ...) \
    ::ntonix::util::Logger::instance().debug(component, __VA_ARGS__)
#define NTONIX_LOG_INFO(component, ...) \
    ::ntonix::util::Logger::instance().info(component, __VA_ARGS__)
#define NTONIX_LOG_WARN(component, ...) \
    ::ntonix::util::Logger::instance().warn(component, __VA_ARGS__)
#define NTONIX_LOG_ERROR(component, ...) \
    ::ntonix::util::Logger::instance().error(component, __VA_ARGS__)
#define NTONIX_LOG_CRITICAL(component, ...) \
    ::ntonix::util::Logger::instance().critical(component, __VA_ARGS__)

// Component constants
namespace log_component {
    constexpr std::string_view Server = "server";
    constexpr std::string_view Config = "config";
    constexpr std::string_view Balancer = "balancer";
    constexpr std::string_view Health = "health";
    constexpr std::string_view Cache = "cache";
    constexpr std::string_view Proxy = "proxy";
    constexpr std::string_view SSL = "ssl";
    constexpr std::string_view Pool = "pool";
}

} // namespace ntonix::util

#endif // NTONIX_UTIL_LOGGER_HPP
