#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <opentelemetry/common/key_value_iterable_view.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/severity.h>
#include <opentelemetry/nostd/span.h>

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "raftpp/fmt.h"

namespace raftpp::logging {

inline std::string_view TrimSourceRoot(std::string_view file) {
#ifdef RAFTPP_SOURCE_ROOT
    constexpr std::string_view kSourceRoot = RAFTPP_SOURCE_ROOT;
    if (file.size() >= kSourceRoot.size() &&
        file.compare(0, kSourceRoot.size(), kSourceRoot) == 0) {
        return file.substr(kSourceRoot.size());
    }
#endif
    return file;
}

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
    auto logger = GetLogger();
    if (!logger) {
        return;
    }
    const auto trimmed_file = TrimSourceRoot(file);
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
                {{"code.filepath", opentelemetry::nostd::string_view{trimmed_file}},
                 {"code.lineno", static_cast<int64_t>(line)}}
            )
        );
        return;
    }
    logger->EmitLogRecord(
        severity, opentelemetry::nostd::string_view{message.data(), message.size()},
        opentelemetry::common::MakeAttributes(
            {{"code.filepath", opentelemetry::nostd::string_view{trimmed_file}},
             {"code.lineno", static_cast<int64_t>(line)}}
        )
    );
}

inline void LogWithLocation(
    opentelemetry::logs::Severity severity, const char* file, int line, std::string_view message
) {
    auto logger = GetLogger();
    if (!logger) {
        return;
    }
    const auto trimmed_file = TrimSourceRoot(file);
    logger->EmitLogRecord(
        severity, opentelemetry::nostd::string_view{message},
        opentelemetry::common::MakeAttributes(
            {{"code.filepath", opentelemetry::nostd::string_view{trimmed_file}},
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
