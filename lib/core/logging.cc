#include "raftpp/logging.h"

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/noop.h>
#include <opentelemetry/nostd/variant.h>
#include <opentelemetry/trace/span_id.h>
#include <opentelemetry/trace/trace_flags.h>
#include <opentelemetry/trace/trace_id.h>
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
    constexpr size_t kMaxAttributeArrayItems = 16;
    auto format_span = [](auto span, auto append_value) {
        std::string out = "[";
        size_t count = 0;
        for (const auto& item : span) {
            if (count >= kMaxAttributeArrayItems) {
                if (count > 0) {
                    out.append(",");
                }
                out.append("...");
                break;
            }
            if (count > 0) {
                out.append(",");
            }
            append_value(out, item);
            ++count;
        }
        out.append("]");
        return out;
    };
    return opentelemetry::nostd::visit(
        [format_span](auto&& v) -> std::string {
            using ValueType = std::decay_t<decltype(v)>;
            using SpanBool = opentelemetry::nostd::span<const bool>;
            using SpanInt32 = opentelemetry::nostd::span<const int32_t>;
            using SpanInt64 = opentelemetry::nostd::span<const int64_t>;
            using SpanUInt32 = opentelemetry::nostd::span<const uint32_t>;
            using SpanDouble = opentelemetry::nostd::span<const double>;
            using SpanStringView =
                opentelemetry::nostd::span<const opentelemetry::nostd::string_view>;
            using SpanUInt64 = opentelemetry::nostd::span<const uint64_t>;
            using SpanUInt8 = opentelemetry::nostd::span<const uint8_t>;

            if constexpr (std::is_same_v<ValueType, const char*>) {
                return v ? std::string(v) : std::string();
            } else if constexpr (std::is_same_v<ValueType, opentelemetry::nostd::string_view>) {
                return std::string(v.data(), v.size());
            } else if constexpr (std::is_same_v<ValueType, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_arithmetic_v<ValueType>) {
                return fmt::format("{}", v);
            } else if constexpr (std::is_same_v<ValueType, SpanBool>) {
                return format_span(v, [](std::string& out, bool item) {
                    out.append(item ? "true" : "false");
                });
            } else if constexpr (std::is_same_v<ValueType, SpanInt32> ||
                                 std::is_same_v<ValueType, SpanInt64> ||
                                 std::is_same_v<ValueType, SpanUInt32> ||
                                 std::is_same_v<ValueType, SpanDouble> ||
                                 std::is_same_v<ValueType, SpanUInt64>) {
                return format_span(v, [](std::string& out, const auto& item) {
                    out.append(fmt::format("{}", item));
                });
            } else if constexpr (std::is_same_v<ValueType, SpanStringView>) {
                return format_span(v, [](std::string& out, opentelemetry::nostd::string_view item) {
                    out.append("\"");
                    out.append(item.data(), item.size());
                    out.append("\"");
                });
            } else if constexpr (std::is_same_v<ValueType, SpanUInt8>) {
                return format_span(v, [](std::string& out, uint8_t item) {
                    out.append(fmt::format("{}", static_cast<unsigned int>(item)));
                });
            } else {
                return "<array>";
            }
        },
        value
    );
}

std::string TraceIdToHex(const opentelemetry::trace::TraceId& trace_id) {
    std::array<char, opentelemetry::trace::TraceId::kSize * 2> buffer{};
    trace_id.ToLowerBase16(
        opentelemetry::nostd::span<char, opentelemetry::trace::TraceId::kSize * 2>(
            buffer.data(), buffer.size()
        )
    );
    return std::string(buffer.data(), buffer.size());
}

std::string SpanIdToHex(const opentelemetry::trace::SpanId& span_id) {
    std::array<char, opentelemetry::trace::SpanId::kSize * 2> buffer{};
    span_id.ToLowerBase16(
        opentelemetry::nostd::span<char, opentelemetry::trace::SpanId::kSize * 2>(
            buffer.data(), buffer.size()
        )
    );
    return std::string(buffer.data(), buffer.size());
}

std::string TraceFlagsToHex(const opentelemetry::trace::TraceFlags& flags) {
    std::array<char, 2> buffer{};
    flags.ToLowerBase16(opentelemetry::nostd::span<char, 2>(buffer.data(), buffer.size()));
    return std::string(buffer.data(), buffer.size());
}

class SpdlogLogRecord final : public opentelemetry::logs::LogRecord {
  public:
    void SetTimestamp(opentelemetry::common::SystemTimestamp timestamp) noexcept override {
        const auto nanos = timestamp.time_since_epoch().count();
        if (nanos == 0) {
            return;
        }
        AddAttribute("otel.time_unix_nano", fmt::format("{}", nanos));
    }

    void SetObservedTimestamp(opentelemetry::common::SystemTimestamp timestamp) noexcept override {
        const auto nanos = timestamp.time_since_epoch().count();
        if (nanos == 0) {
            return;
        }
        AddAttribute("otel.observed_time_unix_nano", fmt::format("{}", nanos));
    }

    void SetSeverity(opentelemetry::logs::Severity severity) noexcept override {
        severity_ = severity;
    }

    void SetBody(const opentelemetry::common::AttributeValue& message) noexcept override {
        body_ = AttributeToString(message);
    }

    void SetAttribute(
        opentelemetry::nostd::string_view key, const opentelemetry::common::AttributeValue& value
    ) noexcept override {
        AddAttribute(std::string(key.data(), key.size()), AttributeToString(value));
    }

    void SetEventId(int64_t id, opentelemetry::nostd::string_view name) noexcept override {
        AddAttribute("otel.event_id", fmt::format("{}", id));
        if (!name.empty()) {
            AddAttribute("otel.event_name", std::string(name.data(), name.size()));
        }
    }

    void SetTraceId(const opentelemetry::trace::TraceId& trace_id) noexcept override {
        if (!trace_id.IsValid()) {
            return;
        }
        AddAttribute("otel.trace_id", TraceIdToHex(trace_id));
    }

    void SetSpanId(const opentelemetry::trace::SpanId& span_id) noexcept override {
        if (!span_id.IsValid()) {
            return;
        }
        AddAttribute("otel.span_id", SpanIdToHex(span_id));
    }

    void SetTraceFlags(const opentelemetry::trace::TraceFlags& trace_flags) noexcept override {
        if (trace_flags.flags() == 0) {
            return;
        }
        AddAttribute("otel.trace_flags", TraceFlagsToHex(trace_flags));
    }

    [[nodiscard]] opentelemetry::logs::Severity severity() const { return severity_; }

    [[nodiscard]] const std::string& body() const { return body_; }

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& attributes() const {
        return attributes_;
    }

  private:
    void AddAttribute(std::string key, std::string value) {
        attributes_.emplace_back(std::move(key), std::move(value));
    }

    opentelemetry::logs::Severity severity_ = opentelemetry::logs::Severity::kInfo;
    std::string body_;
    std::vector<std::pair<std::string, std::string>> attributes_;
};

class SpdlogLogger final : public opentelemetry::logs::Logger {
  public:
    explicit SpdlogLogger(std::string name) : name_(std::move(name)) {}

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
        if (!spdlog_record) {
            spdlog::error("OpenTelemetry log record type mismatch for logger {}", name_);
            return;
        }
        auto backend = spdlog::default_logger();
        if (!backend) {
            spdlog::error("OpenTelemetry log backend unavailable for logger {}", name_);
            return;
        }

        const auto level = ToSpdlogLevel(spdlog_record->severity());
        if (!backend->should_log(level)) {
            return;
        }
        if (spdlog_record->attributes().empty()) {
            backend->log(level, "{}", spdlog_record->body());
            return;
        }

        std::string message = spdlog_record->body();
        for (const auto& [key, value] : spdlog_record->attributes()) {
            message.append(" ");
            message.append(key);
            message.append("=");
            message.append(value);
        }
        backend->log(level, "{}", message);
    }

  private:
    std::string name_;
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
    EnsureProviderInstalled();
    auto provider = opentelemetry::logs::Provider::GetLoggerProvider();
    if (dynamic_cast<opentelemetry::logs::NoopLoggerProvider*>(provider.get()) != nullptr) {
        return false;
    }
    if (dynamic_cast<SpdlogLoggerProvider*>(provider.get()) == nullptr) {
        return true;
    }

    auto backend = spdlog::default_logger();
    if (!backend) {
        return false;
    }
    return backend->should_log(ToSpdlogLevel(severity));
}

opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> GetLogger() {
    EnsureProviderInstalled();
    auto provider = opentelemetry::logs::Provider::GetLoggerProvider();
    thread_local opentelemetry::nostd::shared_ptr<opentelemetry::logs::LoggerProvider>
        cached_provider;
    thread_local opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> logger;
    if (!logger || cached_provider.get() != provider.get()) {
        cached_provider = provider;
        logger = provider->GetLogger("raftpp", "raftpp", "0.1.0");
    }
    return logger;
}

}  // namespace raftpp::logging
