/**
 * NTONIX - High-Performance AI Inference Gateway
 * Logger Implementation
 */

#include "util/logger.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/pattern_formatter.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>

namespace ntonix::util {

// Static members
std::unique_ptr<Logger> Logger::instance_;
std::once_flag Logger::init_flag_;

// Thread-local request context
static thread_local std::string tl_request_id;

void Logger::init(const LogConfig& config) {
    std::call_once(init_flag_, [&config]() {
        instance_ = std::unique_ptr<Logger>(new Logger());
        instance_->configure(config);
    });
}

void Logger::init_default() {
    LogConfig config;
    config.level = LogLevel::Info;
    config.enable_console = true;
    config.enable_colors = true;
    init(config);
}

Logger& Logger::instance() {
    std::call_once(init_flag_, []() {
        instance_ = std::unique_ptr<Logger>(new Logger());
        LogConfig config;
        config.level = LogLevel::Info;
        config.enable_console = true;
        config.enable_colors = true;
        instance_->configure(config);
    });
    return *instance_;
}

Logger::~Logger() {
    shutdown();
}

void Logger::configure(const LogConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<spdlog::sink_ptr> sinks;

    // Console sink
    if (config.enable_console) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        if (!config.enable_colors) {
            console_sink->set_color_mode(spdlog::color_mode::never);
        }
        // Format: [timestamp] [level] message
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        sinks.push_back(console_sink);
    }

    // File sink with rotation
    if (!config.file_path.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.file_path,
            config.max_file_size_mb * 1024 * 1024,
            config.max_files);
        // Format: timestamp level message (no colors in file)
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        sinks.push_back(file_sink);
    }

    // Create main logger
    logger_ = std::make_shared<spdlog::logger>("ntonix", sinks.begin(), sinks.end());
    logger_->set_level(to_spdlog_level(config.level));
    logger_->flush_on(spdlog::level::warn);

    // Create access logger (same sinks but different format for access logs)
    access_logger_ = std::make_shared<spdlog::logger>("access", sinks.begin(), sinks.end());
    access_logger_->set_level(spdlog::level::info); // Access logs always at INFO
    access_logger_->flush_on(spdlog::level::info);

    current_level_.store(config.level, std::memory_order_relaxed);

    // Register loggers
    spdlog::register_logger(logger_);
    spdlog::register_logger(access_logger_);
    spdlog::set_default_logger(logger_);
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logger_) {
        logger_->set_level(to_spdlog_level(level));
    }
    current_level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::get_level() const {
    return current_level_.load(std::memory_order_relaxed);
}

std::optional<LogLevel> Logger::parse_level(std::string_view level_str) {
    std::string lower;
    lower.reserve(level_str.size());
    for (char c : level_str) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower == "trace") return LogLevel::Trace;
    if (lower == "debug") return LogLevel::Debug;
    if (lower == "info") return LogLevel::Info;
    if (lower == "warn" || lower == "warning") return LogLevel::Warn;
    if (lower == "error" || lower == "err") return LogLevel::Error;
    if (lower == "critical" || lower == "crit" || lower == "fatal") return LogLevel::Critical;
    if (lower == "off" || lower == "none") return LogLevel::Off;

    return std::nullopt;
}

std::string_view Logger::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "trace";
        case LogLevel::Debug:    return "debug";
        case LogLevel::Info:     return "info";
        case LogLevel::Warn:     return "warn";
        case LogLevel::Error:    return "error";
        case LogLevel::Critical: return "critical";
        case LogLevel::Off:      return "off";
        default:                 return "unknown";
    }
}

void Logger::access(const AccessLogEntry& entry) {
    if (!access_logger_) return;

    // Format: request_id client_ip "METHOD /path" status response_size latency_ms cache backend
    // Example: abc123 192.168.1.1 "POST /v1/chat/completions" 200 1234 150ms HIT backend1:8001

    std::string cache_status = entry.cache_hit ? "HIT" : "MISS";
    std::string backend_info = entry.backend_host.empty()
        ? "-"
        : fmt::format("{}:{}", entry.backend_host, entry.backend_port);

    access_logger_->info(
        R"({} {} "{} {}" {} {} {}ms {} {})",
        entry.request_id.empty() ? "-" : entry.request_id,
        entry.client_ip.empty() ? "-" : entry.client_ip,
        entry.method,
        entry.path,
        entry.status_code,
        entry.response_size,
        entry.latency.count(),
        cache_status,
        backend_info
    );
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logger_) {
        logger_->flush();
    }
    if (access_logger_) {
        access_logger_->flush();
    }
    spdlog::shutdown();
}

spdlog::level::level_enum Logger::to_spdlog_level(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return spdlog::level::trace;
        case LogLevel::Debug:    return spdlog::level::debug;
        case LogLevel::Info:     return spdlog::level::info;
        case LogLevel::Warn:     return spdlog::level::warn;
        case LogLevel::Error:    return spdlog::level::err;
        case LogLevel::Critical: return spdlog::level::critical;
        case LogLevel::Off:      return spdlog::level::off;
        default:                 return spdlog::level::info;
    }
}

LogLevel Logger::from_spdlog_level(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace:    return LogLevel::Trace;
        case spdlog::level::debug:    return LogLevel::Debug;
        case spdlog::level::info:     return LogLevel::Info;
        case spdlog::level::warn:     return LogLevel::Warn;
        case spdlog::level::err:      return LogLevel::Error;
        case spdlog::level::critical: return LogLevel::Critical;
        case spdlog::level::off:      return LogLevel::Off;
        default:                      return LogLevel::Info;
    }
}

// RequestContext implementation

RequestContext::RequestContext(std::string request_id)
    : request_id_(request_id.empty() ? generate_id() : std::move(request_id))
    , owns_context_(true)
{
    tl_request_id = request_id_;
}

RequestContext::~RequestContext() {
    if (owns_context_) {
        tl_request_id.clear();
    }
}

RequestContext::RequestContext(RequestContext&& other) noexcept
    : request_id_(std::move(other.request_id_))
    , owns_context_(other.owns_context_)
{
    other.owns_context_ = false;
}

RequestContext& RequestContext::operator=(RequestContext&& other) noexcept {
    if (this != &other) {
        if (owns_context_) {
            tl_request_id.clear();
        }
        request_id_ = std::move(other.request_id_);
        owns_context_ = other.owns_context_;
        other.owns_context_ = false;
        if (owns_context_) {
            tl_request_id = request_id_;
        }
    }
    return *this;
}

std::string RequestContext::current_id() {
    return tl_request_id;
}

std::string RequestContext::generate_id() {
    // Generate a random 16-character hex ID
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static constexpr char hex_chars[] = "0123456789abcdef";

    std::uint64_t value = rng();
    std::string id;
    id.reserve(16);

    for (int i = 0; i < 16; ++i) {
        id.push_back(hex_chars[(value >> (i * 4)) & 0xF]);
    }

    return id;
}

} // namespace ntonix::util
