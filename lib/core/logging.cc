#include "raftpp/logging.h"

#include <cctype>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace raftpp::logging {
namespace {

std::mutex g_logger_mutex;

std::string ToString(std::string_view value) {
    return {value.data(), value.size()};
}

std::optional<LogLevel> ParseLogLevel(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }

    if (normalized == "trace") {
        return LogLevel::kTrace;
    }
    if (normalized == "debug") {
        return LogLevel::kDebug;
    }
    if (normalized == "info") {
        return LogLevel::kInfo;
    }
    if (normalized == "warn" || normalized == "warning") {
        return LogLevel::kWarn;
    }
    if (normalized == "error" || normalized == "err") {
        return LogLevel::kError;
    }
    if (normalized == "critical" || normalized == "fatal") {
        return LogLevel::kCritical;
    }
    if (normalized == "off") {
        return LogLevel::kOff;
    }
    return std::nullopt;
}

}  // namespace

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::kTrace:
            return spdlog::level::trace;
        case LogLevel::kDebug:
            return spdlog::level::debug;
        case LogLevel::kInfo:
            return spdlog::level::info;
        case LogLevel::kWarn:
            return spdlog::level::warn;
        case LogLevel::kError:
            return spdlog::level::err;
        case LogLevel::kCritical:
            return spdlog::level::critical;
        case LogLevel::kOff:
            return spdlog::level::off;
    }
    return spdlog::level::info;
}

std::shared_ptr<spdlog::logger> GetLogger(std::string_view logger_name) {
    const std::string name =
        ToString(logger_name.empty() ? std::string_view("raftpp") : logger_name);
    if (auto logger = spdlog::get(name); logger != nullptr) {
        return logger;
    }

    std::lock_guard lock(g_logger_mutex);
    if (auto logger = spdlog::get(name); logger != nullptr) {
        return logger;
    }

    auto logger = spdlog::default_logger()->clone(name);
    logger->set_level(spdlog::level::trace);
    spdlog::register_logger(logger);
    return logger;
}

spdlog::logger* GetLoggerRaw(std::string_view logger_name) {
    return GetLogger(logger_name).get();
}

void SetLogger(std::string logger_name, std::shared_ptr<spdlog::logger> logger) {
    if (logger == nullptr) {
        return;
    }

    std::lock_guard lock(g_logger_mutex);
    logger->set_level(spdlog::level::trace);
    if (spdlog::get(logger_name) != nullptr) {
        spdlog::drop(logger_name);
    }
    spdlog::register_logger(std::move(logger));
}

void SetLogLevel(LogLevel level) {
    SetLoggerLevel("raftpp", level);
}

void SetLoggerLevel(std::string_view logger_name, LogLevel level) {
    GetLogger(logger_name)->set_level(ToSpdlogLevel(level));
}

void ConfigureFromEnv(LogLevel default_level) {
    ConfigureLoggerFromEnv("raftpp", "RAFTPP_LOG_LEVEL", default_level);
}

void ConfigureLoggerFromEnv(
    std::string_view logger_name, std::string_view env_name, LogLevel default_level
) {
    LogLevel level = default_level;
    const std::string env = ToString(env_name);
    if (const char* value = std::getenv(env.c_str()); value != nullptr) {
        if (auto parsed = ParseLogLevel(value); parsed.has_value()) {
            level = *parsed;
        }
    }
    SetLoggerLevel(logger_name, level);
}

bool ShouldLog(std::string_view logger_name, LogLevel level) {
    return GetLogger(logger_name)->should_log(ToSpdlogLevel(level));
}

}  // namespace raftpp::logging
