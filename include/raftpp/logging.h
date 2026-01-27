#pragma once

#include <cstdint>
#include <exception>
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
    auto logger = GetLogger();
    if (!logger) {
        return;
    }
    std::string message;
    try {
        message = fmt::vformat(format, fmt::make_format_args(args...));
    } catch (const std::exception& ex) {
        message = "Log formatting failed: ";
        message.append(ex.what());
        message.append(" format=");
        message.append(format.data(), format.size());
        logger->EmitLogRecord(
            opentelemetry::logs::Severity::kError,
            opentelemetry::nostd::string_view{message.data(), message.size()},
            opentelemetry::common::MakeAttributes(
                {{"code.filepath", opentelemetry::nostd::string_view{file}},
                 {"code.lineno", static_cast<int64_t>(line)}}
            )
        );
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

#define RAFTPP_LOG_DEBUG(...)                                                            \
    do {                                                                                 \
        if (::raftpp::logging::ShouldLog(::opentelemetry::logs::Severity::kDebug))       \
            ::raftpp::logging::LogWithLocation(                                          \
                ::opentelemetry::logs::Severity::kDebug, __FILE__, __LINE__, __VA_ARGS__ \
            );                                                                           \
    } while (0)
#define RAFTPP_LOG_INFO(...)                                                            \
    do {                                                                                \
        if (::raftpp::logging::ShouldLog(::opentelemetry::logs::Severity::kInfo))       \
            ::raftpp::logging::LogWithLocation(                                         \
                ::opentelemetry::logs::Severity::kInfo, __FILE__, __LINE__, __VA_ARGS__ \
            );                                                                          \
    } while (0)
#define RAFTPP_LOG_WARN(...)                                                            \
    do {                                                                                \
        if (::raftpp::logging::ShouldLog(::opentelemetry::logs::Severity::kWarn))       \
            ::raftpp::logging::LogWithLocation(                                         \
                ::opentelemetry::logs::Severity::kWarn, __FILE__, __LINE__, __VA_ARGS__ \
            );                                                                          \
    } while (0)
#define RAFTPP_LOG_ERROR(...)                                                            \
    do {                                                                                 \
        if (::raftpp::logging::ShouldLog(::opentelemetry::logs::Severity::kError))       \
            ::raftpp::logging::LogWithLocation(                                          \
                ::opentelemetry::logs::Severity::kError, __FILE__, __LINE__, __VA_ARGS__ \
            );                                                                           \
    } while (0)
#define RAFTPP_LOG_FATAL(...)                                                            \
    do {                                                                                 \
        if (::raftpp::logging::ShouldLog(::opentelemetry::logs::Severity::kFatal))       \
            ::raftpp::logging::LogWithLocation(                                          \
                ::opentelemetry::logs::Severity::kFatal, __FILE__, __LINE__, __VA_ARGS__ \
            );                                                                           \
    } while (0)
#define RAFTPP_LOG_CRITICAL(...) RAFTPP_LOG_FATAL(__VA_ARGS__)
