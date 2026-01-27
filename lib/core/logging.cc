#include "raftpp/logging.h"

#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/noop.h>
#include <opentelemetry/nostd/variant.h>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>

namespace raftpp::logging {
namespace {

using opentelemetry::logs::Severity;

spdlog::level::level_enum ToSpdlogLevel(Severity severity) {
    switch (severity) {
        case Severity::kTrace:
        case Severity::kTrace2:
        case Severity::kTrace3:
        case Severity::kTrace4:
            return spdlog::level::trace;
        case Severity::kDebug:
        case Severity::kDebug2:
        case Severity::kDebug3:
        case Severity::kDebug4:
            return spdlog::level::debug;
        case Severity::kInfo:
        case Severity::kInfo2:
        case Severity::kInfo3:
        case Severity::kInfo4:
            return spdlog::level::info;
        case Severity::kWarn:
        case Severity::kWarn2:
        case Severity::kWarn3:
        case Severity::kWarn4:
            return spdlog::level::warn;
        case Severity::kError:
        case Severity::kError2:
        case Severity::kError3:
        case Severity::kError4:
            return spdlog::level::err;
        case Severity::kFatal:
        case Severity::kFatal2:
        case Severity::kFatal3:
        case Severity::kFatal4:
            return spdlog::level::critical;
        case Severity::kInvalid:
            return spdlog::level::debug;
    }
    return spdlog::level::debug;
}

std::string AttributeToString(const opentelemetry::common::AttributeValue& value) {
    return opentelemetry::nostd::visit(
        [](auto&& v) -> std::string {
            using ValueType = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<ValueType, const char*>) {
                return v ? std::string(v) : std::string();
            } else if constexpr (std::is_same_v<ValueType, opentelemetry::nostd::string_view>) {
                return std::string(v.data(), v.size());
            } else if constexpr (std::is_same_v<ValueType, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_arithmetic_v<ValueType>) {
                return fmt::format("{}", v);
            } else {
                return "<array>";
            }
        },
        value
    );
}

class SpdlogLogRecord final : public opentelemetry::logs::LogRecord {
  public:
    void SetTimestamp(opentelemetry::common::SystemTimestamp /*timestamp*/) noexcept override {}

    void
    SetObservedTimestamp(opentelemetry::common::SystemTimestamp /*timestamp*/) noexcept override {}

    void SetSeverity(opentelemetry::logs::Severity severity) noexcept override {
        severity_ = severity;
    }

    void SetBody(const opentelemetry::common::AttributeValue& message) noexcept override {
        body_ = AttributeToString(message);
    }

    void SetAttribute(
        opentelemetry::nostd::string_view key, const opentelemetry::common::AttributeValue& value
    ) noexcept override {
        attributes_.emplace_back(std::string(key.data(), key.size()), AttributeToString(value));
    }

    void SetEventId(int64_t /*id*/, opentelemetry::nostd::string_view /*name*/) noexcept override {}

    void SetTraceId(const opentelemetry::trace::TraceId& /*trace_id*/) noexcept override {}

    void SetSpanId(const opentelemetry::trace::SpanId& /*span_id*/) noexcept override {}

    void SetTraceFlags(const opentelemetry::trace::TraceFlags& /*trace_flags*/) noexcept override {}

    [[nodiscard]] opentelemetry::logs::Severity severity() const { return severity_; }

    [[nodiscard]] const std::string& body() const { return body_; }

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& attributes() const {
        return attributes_;
    }

  private:
    opentelemetry::logs::Severity severity_ = opentelemetry::logs::Severity::kInfo;
    std::string body_;
    std::vector<std::pair<std::string, std::string>> attributes_;
};

class SpdlogLogger final : public opentelemetry::logs::Logger {
  public:
    explicit SpdlogLogger(std::string name)
        : name_(std::move(name)), backend_(spdlog::default_logger()) {}

    const opentelemetry::nostd::string_view GetName() noexcept override { return name_; }

    opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>
    CreateLogRecord() noexcept override {
        return opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>(
            new SpdlogLogRecord()
        );
    }

    using Logger::EmitLogRecord;

    void EmitLogRecord(
        opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>&& record
    ) noexcept override {
        auto* spdlog_record = dynamic_cast<SpdlogLogRecord*>(record.get());
        if (!spdlog_record || !backend_) {
            return;
        }

        const auto level = ToSpdlogLevel(spdlog_record->severity());
        if (!backend_->should_log(level)) {
            return;
        }
        if (spdlog_record->attributes().empty()) {
            backend_->log(level, spdlog_record->body());
            return;
        }

        std::string message = spdlog_record->body();
        for (const auto& [key, value] : spdlog_record->attributes()) {
            message.append(" ");
            message.append(key);
            message.append("=");
            message.append(value);
        }
        backend_->log(level, message);
    }

  private:
    std::string name_;
    std::shared_ptr<spdlog::logger> backend_;
};

class SpdlogLoggerProvider final : public opentelemetry::logs::LoggerProvider {
  public:
    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> GetLogger(
        opentelemetry::nostd::string_view logger_name,
        opentelemetry::nostd::string_view /*library_name*/,
        opentelemetry::nostd::string_view /*library_version*/,
        opentelemetry::nostd::string_view /*schema_url*/,
        const opentelemetry::common::KeyValueIterable& /*attributes*/
    ) override {
        return opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger>(
            new SpdlogLogger(std::string(logger_name.data(), logger_name.size()))
        );
    }
};

void EnsureProviderInstalled() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto provider = opentelemetry::logs::Provider::GetLoggerProvider();
        if (dynamic_cast<opentelemetry::logs::NoopLoggerProvider*>(provider.get()) != nullptr) {
            opentelemetry::logs::Provider::SetLoggerProvider(
                opentelemetry::nostd::shared_ptr<opentelemetry::logs::LoggerProvider>(
                    new SpdlogLoggerProvider()
                )
            );
        }
    });
}

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

}  // namespace

void SetLogLevel(LogLevel level) {
    spdlog::set_level(ToSpdlogLevel(level));
}

void ConfigureFromEnv(LogLevel default_level) {
    spdlog::set_level(ToSpdlogLevel(default_level));
    spdlog::cfg::load_env_levels();
}

bool ShouldLog(opentelemetry::logs::Severity severity) {
    auto backend = spdlog::default_logger();
    if (!backend) {
        return false;
    }
    return backend->should_log(ToSpdlogLevel(severity));
}

opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> GetLogger() {
    static std::once_flag once;
    static opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> logger;
    std::call_once(once, [] {
        EnsureProviderInstalled();
        auto provider = opentelemetry::logs::Provider::GetLoggerProvider();
        logger = provider->GetLogger("raftpp", "raftpp", "0.1.0");
    });
    return logger;
}

}  // namespace raftpp::logging
