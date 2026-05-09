#include "raftpp/logging.h"

#include <memory>
#include <string>

#include <doctest/doctest.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

namespace {

struct CapturedLogRecord {
    std::string logger_name;
    spdlog::level::level_enum level = spdlog::level::off;
    std::string message;
    std::string filepath;
    int line = 0;
};

class CapturingSink final : public spdlog::sinks::sink {
  public:
    explicit CapturingSink(CapturedLogRecord* captured) : captured_(captured) {}

    void log(const spdlog::details::log_msg& msg) override {
        captured_->logger_name = std::string(msg.logger_name.data(), msg.logger_name.size());
        captured_->level = msg.level;
        captured_->message = std::string(msg.payload.data(), msg.payload.size());
        if (msg.source.filename != nullptr) {
            captured_->filepath = msg.source.filename;
        }
        captured_->line = msg.source.line;
    }

    void flush() override {}

    void set_pattern(const std::string& /*pattern*/) override {}

    void set_formatter(std::unique_ptr<spdlog::formatter> /*sink_formatter*/) override {}

  private:
    CapturedLogRecord* captured_;
};

class ScopedLogger {
  public:
    ScopedLogger(std::string name, CapturedLogRecord* captured) : name_(std::move(name)) {
        logger_ =
            std::make_shared<spdlog::logger>(name_, std::make_shared<CapturingSink>(captured));
        logger_->set_level(spdlog::level::trace);
        raftpp::logging::SetLogger(name_, logger_);
    }

    ~ScopedLogger() { spdlog::drop(name_); }

    ScopedLogger(const ScopedLogger&) = delete;
    ScopedLogger& operator=(const ScopedLogger&) = delete;

  private:
    std::string name_;
    std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace

TEST_SUITE_BEGIN("logging");

TEST_CASE("logging: formatted logs trim repository root from code filepath") {
    CapturedLogRecord captured;
    ScopedLogger logger("test", &captured);

    const std::string absolute_path = std::string(RAFTPP_SOURCE_ROOT) + "include/raftpp/logging.h";
    raftpp::logging::LogWithLocation(
        "test", raftpp::logging::LogLevel::kInfo, absolute_path.c_str(), 42, "test", "hello {}",
        "raftpp"
    );

    CHECK_EQ("test", captured.logger_name);
    CHECK_EQ(spdlog::level::info, captured.level);
    CHECK_EQ("hello raftpp", captured.message);
    CHECK_EQ("include/raftpp/logging.h", captured.filepath);
    CHECK_EQ(42, captured.line);
}

TEST_CASE("logging: plain message logs keep external filepath unchanged") {
    CapturedLogRecord captured;
    ScopedLogger logger("test", &captured);

    constexpr const char* kExternalPath = "/tmp/external/file.cc";
    raftpp::logging::Log(
        "test", raftpp::logging::LogLevel::kWarn, kExternalPath, 7, "test", "external log"
    );

    CHECK_EQ(spdlog::level::warn, captured.level);
    CHECK_EQ("external log", captured.message);
    CHECK_EQ(kExternalPath, captured.filepath);
    CHECK_EQ(7, captured.line);
}

TEST_CASE("logging: level filtering is delegated to spdlog logger") {
    CapturedLogRecord captured;
    ScopedLogger logger("test", &captured);

    raftpp::logging::SetLoggerLevel("test", raftpp::logging::LogLevel::kError);
    CHECK_FALSE(raftpp::logging::ShouldLog("test", raftpp::logging::LogLevel::kInfo));
    CHECK(raftpp::logging::ShouldLog("test", raftpp::logging::LogLevel::kError));
}

TEST_CASE("logging: created loggers default to warn level") {
    constexpr const char* kLoggerName = "test-default-level";
    spdlog::drop(kLoggerName);

    CHECK_FALSE(raftpp::logging::ShouldLog(kLoggerName, raftpp::logging::LogLevel::kInfo));
    CHECK(raftpp::logging::ShouldLog(kLoggerName, raftpp::logging::LogLevel::kWarn));

    spdlog::drop(kLoggerName);
}

TEST_CASE("logging: SetLogger preserves caller configured level") {
    constexpr const char* kLoggerName = "test-preserve-level";
    auto logger = std::make_shared<spdlog::logger>(
        kLoggerName, std::make_shared<spdlog::sinks::null_sink_mt>()
    );
    logger->set_level(spdlog::level::err);

    raftpp::logging::SetLogger(kLoggerName, logger);

    CHECK_FALSE(raftpp::logging::ShouldLog(kLoggerName, raftpp::logging::LogLevel::kWarn));
    CHECK(raftpp::logging::ShouldLog(kLoggerName, raftpp::logging::LogLevel::kError));

    spdlog::drop(kLoggerName);
}

TEST_SUITE_END();
