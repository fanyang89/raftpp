#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

namespace raftpp::logging {

constexpr const char* TrimSourceRoot(const char* file) {
    const std::string_view path(file);
#ifdef RAFTPP_SOURCE_ROOT
    constexpr std::string_view kSourceRoot = RAFTPP_SOURCE_ROOT;
    if (path.size() >= kSourceRoot.size() &&
        path.compare(0, kSourceRoot.size(), kSourceRoot) == 0) {
        return file + kSourceRoot.size();
    }
#endif
#ifdef PULPFS_SOURCE_ROOT
    constexpr std::string_view kAppSourceRoot = PULPFS_SOURCE_ROOT;
    if (path.size() >= kAppSourceRoot.size() &&
        path.compare(0, kAppSourceRoot.size(), kAppSourceRoot) == 0) {
        return file + kAppSourceRoot.size();
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

spdlog::level::level_enum ToSpdlogLevel(LogLevel level);
std::shared_ptr<spdlog::logger> GetLogger(std::string_view logger_name = "raftpp");
spdlog::logger* GetLoggerRaw(std::string_view logger_name = "raftpp");
void SetLogger(std::string logger_name, std::shared_ptr<spdlog::logger> logger);
void SetLogLevel(LogLevel level);
void SetLoggerLevel(std::string_view logger_name, LogLevel level);
void ConfigureFromEnv(LogLevel default_level = LogLevel::kWarn);
void ConfigureLoggerFromEnv(
    std::string_view logger_name, std::string_view env_name, LogLevel default_level
);
bool ShouldLog(std::string_view logger_name, LogLevel level);

template <typename... Args>
inline void LogWithLocation(
    std::string_view logger_name, LogLevel level, const char* file, int line, const char* function,
    spdlog::format_string_t<Args...> format, Args&&... args
) {
    GetLoggerRaw(logger_name)
        ->log(
            spdlog::source_loc{TrimSourceRoot(file), line, function}, ToSpdlogLevel(level), format,
            std::forward<Args>(args)...
        );
}

inline void Log(
    std::string_view logger_name, LogLevel level, const char* file, int line, const char* function,
    spdlog::string_view_t message
) {
    GetLoggerRaw(logger_name)
        ->log(
            spdlog::source_loc{TrimSourceRoot(file), line, function}, ToSpdlogLevel(level), message
        );
}

}  // namespace raftpp::logging

#define RAFTPP_LOGGER_CALL(logger_name, level, ...)                           \
    do {                                                                      \
        if (::raftpp::logging::ShouldLog(logger_name, level)) {               \
            ::raftpp::logging::LogWithLocation(                               \
                logger_name, level, __FILE__, __LINE__, __func__, __VA_ARGS__ \
            );                                                                \
        }                                                                     \
    } while (0)

#define RAFTPP_LOG_TRACE(...) \
    RAFTPP_LOGGER_CALL("raftpp", ::raftpp::logging::LogLevel::kTrace, __VA_ARGS__)
#define RAFTPP_LOG_DEBUG(...) \
    RAFTPP_LOGGER_CALL("raftpp", ::raftpp::logging::LogLevel::kDebug, __VA_ARGS__)
#define RAFTPP_LOG_INFO(...) \
    RAFTPP_LOGGER_CALL("raftpp", ::raftpp::logging::LogLevel::kInfo, __VA_ARGS__)
#define RAFTPP_LOG_WARN(...) \
    RAFTPP_LOGGER_CALL("raftpp", ::raftpp::logging::LogLevel::kWarn, __VA_ARGS__)
#define RAFTPP_LOG_ERROR(...) \
    RAFTPP_LOGGER_CALL("raftpp", ::raftpp::logging::LogLevel::kError, __VA_ARGS__)
#define RAFTPP_LOG_FATAL(...) \
    RAFTPP_LOGGER_CALL("raftpp", ::raftpp::logging::LogLevel::kCritical, __VA_ARGS__)
#define RAFTPP_LOG_CRITICAL(...) RAFTPP_LOG_FATAL(__VA_ARGS__)
