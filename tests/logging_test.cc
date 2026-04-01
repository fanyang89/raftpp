#include "raftpp/logging.h"

#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

#include <doctest/doctest.h>
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/common/timestamp.h>
#include <opentelemetry/logs/log_record.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/logger_provider.h>
#include <opentelemetry/logs/provider.h>
#include <opentelemetry/logs/severity.h>

#include "fmt/format.h"
#include "opentelemetry/nostd/unique_ptr.h"
#include "opentelemetry/nostd/variant.h"

namespace {

struct CapturedLogRecord {
    opentelemetry::logs::Severity severity = opentelemetry::logs::Severity::kInvalid;
    std::string body;
    std::string filepath;
    int64_t line = 0;
};

std::string AttributeValueToString(const opentelemetry::common::AttributeValue& value) {
    return opentelemetry::nostd::visit(
        [](auto&& v) -> std::string {
            using ValueType = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<ValueType, const char*>) {
                return v != nullptr ? std::string(v) : std::string();
            } else if constexpr (std::is_same_v<ValueType, opentelemetry::nostd::string_view>) {
                return std::string(v.data(), v.size());
            } else if constexpr (std::is_integral_v<ValueType> &&
                                 !std::is_same_v<ValueType, bool>) {
                return std::to_string(v);
            } else {
                return {};
            }
        },
        value
    );
}

int64_t AttributeValueToInt64(const opentelemetry::common::AttributeValue& value) {
    return opentelemetry::nostd::visit(
        [](auto&& v) -> int64_t {
            using ValueType = std::decay_t<decltype(v)>;
            if constexpr (std::is_integral_v<ValueType> && !std::is_same_v<ValueType, bool>) {
                return static_cast<int64_t>(v);
            } else {
                return 0;
            }
        },
        value
    );
}

class CapturingLogRecord final : public opentelemetry::logs::LogRecord {
  public:
    explicit CapturingLogRecord(CapturedLogRecord* captured) : captured_(captured) {}

    void SetTimestamp(opentelemetry::common::SystemTimestamp /*timestamp*/) noexcept override {}

    void SetObservedTimestamp(opentelemetry::common::SystemTimestamp /*timestamp*/) noexcept
        override {}

    void SetSeverity(opentelemetry::logs::Severity severity) noexcept override {
        captured_->severity = severity;
    }

    void SetBody(const opentelemetry::common::AttributeValue& message) noexcept override {
        captured_->body = AttributeValueToString(message);
    }

    void SetAttribute(
        opentelemetry::nostd::string_view key, const opentelemetry::common::AttributeValue& value
    ) noexcept override {
        const std::string key_string(key.data(), key.size());
        if (key_string == "code.filepath") {
            captured_->filepath = AttributeValueToString(value);
            return;
        }
        if (key_string == "code.lineno") {
            captured_->line = AttributeValueToInt64(value);
        }
    }

    void SetEventId(
        int64_t /*id*/, opentelemetry::nostd::string_view /*name*/ = {}
    ) noexcept override {}

    void SetTraceId(const opentelemetry::trace::TraceId& /*trace_id*/) noexcept override {}

    void SetSpanId(const opentelemetry::trace::SpanId& /*span_id*/) noexcept override {}

    void SetTraceFlags(const opentelemetry::trace::TraceFlags& /*trace_flags*/
    ) noexcept override {}

  private:
    CapturedLogRecord* captured_;
};

class CapturingLogger final : public opentelemetry::logs::Logger {
  public:
    explicit CapturingLogger(CapturedLogRecord* captured) : captured_(captured) {}

    const opentelemetry::nostd::string_view GetName() noexcept override { return "test"; }

    opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord> CreateLogRecord(
    ) noexcept override {
        return opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>(
            new CapturingLogRecord(captured_)
        );
    }

    using Logger::EmitLogRecord;

    void EmitLogRecord(
        opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>&& /*log_record*/
    ) noexcept override {}

  private:
    CapturedLogRecord* captured_;
};

class CapturingLoggerProvider final : public opentelemetry::logs::LoggerProvider {
  public:
    explicit CapturingLoggerProvider(CapturedLogRecord* captured)
        : logger_(opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger>(
              new CapturingLogger(captured)
          )) {}

    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> GetLogger(
        opentelemetry::nostd::string_view /*logger_name*/,
        opentelemetry::nostd::string_view /*library_name*/,
        opentelemetry::nostd::string_view /*library_version*/,
        opentelemetry::nostd::string_view /*schema_url*/,
        const opentelemetry::common::KeyValueIterable& /*attributes*/
    ) override {
        return logger_;
    }

  private:
    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> logger_;
};

class ScopedLoggerProvider {
  public:
    explicit ScopedLoggerProvider(CapturedLogRecord* captured)
        : previous_(opentelemetry::logs::Provider::GetLoggerProvider()) {
        opentelemetry::logs::Provider::SetLoggerProvider(
            opentelemetry::nostd::shared_ptr<opentelemetry::logs::LoggerProvider>(
                new CapturingLoggerProvider(captured)
            )
        );
    }

    ~ScopedLoggerProvider() { opentelemetry::logs::Provider::SetLoggerProvider(previous_); }

    ScopedLoggerProvider(const ScopedLoggerProvider&) = delete;
    ScopedLoggerProvider& operator=(const ScopedLoggerProvider&) = delete;

  private:
    opentelemetry::nostd::shared_ptr<opentelemetry::logs::LoggerProvider> previous_;
};

class ScopedLogLevel {
  public:
    explicit ScopedLogLevel(const raftpp::logging::LogLevel level) {
        raftpp::logging::SetLogLevel(level);
    }

    ~ScopedLogLevel() { raftpp::logging::SetLogLevel(raftpp::logging::LogLevel::kWarn); }

    ScopedLogLevel(const ScopedLogLevel&) = delete;
    ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;
};

class ScopedStderrCapture {
  public:
    ScopedStderrCapture() {
        std::fflush(stderr);
        int pipe_fds[2] = {-1, -1};
        REQUIRE(::pipe(pipe_fds) == 0);
        read_fd_ = pipe_fds[0];
        saved_fd_ = ::dup(STDERR_FILENO);
        REQUIRE(saved_fd_ >= 0);
        REQUIRE(::dup2(pipe_fds[1], STDERR_FILENO) >= 0);
        REQUIRE(::close(pipe_fds[1]) == 0);
    }

    ~ScopedStderrCapture() {
        Restore();
        if (read_fd_ >= 0) {
            ::close(read_fd_);
        }
    }

    std::string output() {
        Restore();

        std::string out;
        char buffer[256];
        while (true) {
            const ssize_t bytes_read = ::read(read_fd_, buffer, sizeof(buffer));
            if (bytes_read > 0) {
                out.append(buffer, static_cast<size_t>(bytes_read));
                continue;
            }

            if (bytes_read == 0) {
                break;
            }

            const int read_errno = errno;
            if (read_errno == EINTR) {
                continue;
            }

            REQUIRE_MESSAGE(false, "stderr capture read failed with errno=", read_errno);
            return out;
        }

        REQUIRE(::close(read_fd_) == 0);
        read_fd_ = -1;
        return out;
    }

    ScopedStderrCapture(const ScopedStderrCapture&) = delete;
    ScopedStderrCapture& operator=(const ScopedStderrCapture&) = delete;

  private:
    void Restore() {
        if (saved_fd_ < 0) {
            return;
        }
        std::fflush(stderr);
        REQUIRE(::dup2(saved_fd_, STDERR_FILENO) >= 0);
        REQUIRE(::close(saved_fd_) == 0);
        saved_fd_ = -1;
    }

    int read_fd_ = -1;
    int saved_fd_ = -1;
};

}  // namespace

TEST_SUITE_BEGIN("logging");

TEST_CASE("logging: formatted logs trim repository root from code filepath") {
    CapturedLogRecord captured;
    ScopedLoggerProvider provider(&captured);

    const std::string absolute_path = std::string(RAFTPP_SOURCE_ROOT) + "include/raftpp/logging.h";
    raftpp::logging::LogWithLocation(
        opentelemetry::logs::Severity::kInfo, absolute_path.c_str(), 42, "hello {}", "raftpp"
    );

    CHECK_EQ(opentelemetry::logs::Severity::kInfo, captured.severity);
    CHECK_EQ("hello raftpp", captured.body);
    CHECK_EQ("include/raftpp/logging.h", captured.filepath);
    CHECK_EQ(42, captured.line);
}

TEST_CASE("logging: plain message logs keep external filepath unchanged") {
    CapturedLogRecord captured;
    ScopedLoggerProvider provider(&captured);

    constexpr const char* kExternalPath = "/tmp/external/file.cc";
    raftpp::logging::LogWithLocation(
        opentelemetry::logs::Severity::kWarn, kExternalPath, 7, std::string_view("external log")
    );

    CHECK_EQ(opentelemetry::logs::Severity::kWarn, captured.severity);
    CHECK_EQ("external log", captured.body);
    CHECK_EQ(kExternalPath, captured.filepath);
    CHECK_EQ(7, captured.line);
}

TEST_CASE("logging: stderr output renders repository file as clickable basename") {
    ScopedLogLevel log_level(raftpp::logging::LogLevel::kTrace);
    ScopedStderrCapture capture;

    const std::string absolute_path = std::string(RAFTPP_SOURCE_ROOT) + "lib/raftor/raftor.cc";
    raftpp::logging::LogWithLocation(
        opentelemetry::logs::Severity::kInfo, absolute_path.c_str(), 123,
        std::string_view("clickable log")
    );

    CHECK(capture.output().find("[raftor.cc:123] clickable log") != std::string::npos);
}

TEST_CASE("logging: stderr output keeps external absolute filepath") {
    ScopedLogLevel log_level(raftpp::logging::LogLevel::kTrace);
    ScopedStderrCapture capture;

    constexpr const char* kExternalPath = "/tmp/external/file.cc";
    raftpp::logging::LogWithLocation(
        opentelemetry::logs::Severity::kWarn, kExternalPath, 7, std::string_view("external log")
    );

    CHECK(capture.output().find("[/tmp/external/file.cc:7] external log") != std::string::npos);
}

TEST_SUITE_END();
