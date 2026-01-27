#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <opentelemetry/common/key_value_iterable_view.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/provider.h>
#include <opentelemetry/logs/severity.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/string_view.h>
#include <spdlog/fmt/fmt.h>

namespace raftpp::logging {

enum class LogLevel {
    kTrace,
    kDebug,
    kInfo,
    kWarn,
    kError,
    kCritical,
    kOff,
};

void SetLogLevel(LogLevel level);
void ConfigureFromEnv(LogLevel default_level = LogLevel::kWarn);

opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> GetLogger();
bool ShouldLog(opentelemetry::logs::Severity severity);

template <typename... Args>
inline void LogWithLocation(
    opentelemetry::logs::Severity severity, const char* file, int line, fmt::string_view format,
    Args&&... args
) {
    if (!ShouldLog(severity)) {
        return;
    }
    auto message = fmt::vformat(format, fmt::make_format_args(args...));
    auto logger = GetLogger();
    if (!logger) {
        return;
    }
    logger->EmitLogRecord(
        severity, opentelemetry::nostd::string_view{message.data(), message.size()},
        opentelemetry::common::MakeAttributes(
            {{"code.filepath", opentelemetry::nostd::string_view{file}},
             {"code.lineno", static_cast<int64_t>(line)}}
        )
    );
}

inline void LogWithLocation(
    opentelemetry::logs::Severity severity, const char* file, int line, std::string_view message
) {
    if (!ShouldLog(severity)) {
        return;
    }
    auto logger = GetLogger();
    if (!logger) {
        return;
    }
    logger->EmitLogRecord(
        severity, opentelemetry::nostd::string_view{message},
        opentelemetry::common::MakeAttributes(
            {{"code.filepath", opentelemetry::nostd::string_view{file}},
             {"code.lineno", static_cast<int64_t>(line)}}
        )
    );
}

}  // namespace raftpp::logging

#define RAFTPP_LOG_DEBUG(...)                                                    \
    ::raftpp::logging::LogWithLocation(                                          \
        ::opentelemetry::logs::Severity::kDebug, __FILE__, __LINE__, __VA_ARGS__ \
    )
#define RAFTPP_LOG_INFO(...)                                                    \
    ::raftpp::logging::LogWithLocation(                                         \
        ::opentelemetry::logs::Severity::kInfo, __FILE__, __LINE__, __VA_ARGS__ \
    )
#define RAFTPP_LOG_WARN(...)                                                    \
    ::raftpp::logging::LogWithLocation(                                         \
        ::opentelemetry::logs::Severity::kWarn, __FILE__, __LINE__, __VA_ARGS__ \
    )
#define RAFTPP_LOG_ERROR(...)                                                    \
    ::raftpp::logging::LogWithLocation(                                          \
        ::opentelemetry::logs::Severity::kError, __FILE__, __LINE__, __VA_ARGS__ \
    )
