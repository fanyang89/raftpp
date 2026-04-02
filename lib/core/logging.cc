#include "raftpp/logging.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/common/timestamp.h>
#include <opentelemetry/logs/log_record.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/logger_provider.h>
#include <opentelemetry/logs/noop.h>
#include <opentelemetry/logs/provider.h>
#include <opentelemetry/trace/span_id.h>
#include <opentelemetry/trace/trace_flags.h>
#include <opentelemetry/trace/trace_id.h>

#include "opentelemetry/nostd/unique_ptr.h"
#include "opentelemetry/nostd/variant.h"

namespace raftpp::logging {
namespace {

using opentelemetry::logs::Severity;

std::atomic<LogLevel> g_log_level{LogLevel::kWarn};
std::mutex g_log_mutex;

std::string_view Basename(const std::string_view path) {
    const size_t last_separator = path.find_last_of('/');
    if (last_separator == std::string_view::npos) {
        return path;
    }
    return path.substr(last_separator + 1);
}

std::string FormatCodeLocation(const std::string_view filepath, const std::string_view line) {
    if (filepath.empty() || line.empty()) {
        return {};
    }

    const std::string_view rendered_path = filepath.front() == '/' ? filepath : Basename(filepath);
    return fmt::format("[{}:{}]", rendered_path, line);
}

int LogLevelRank(const LogLevel level) {
    switch (level) {
        case LogLevel::kTrace:
            return 0;
        case LogLevel::kDebug:
            return 1;
        case LogLevel::kInfo:
            return 2;
        case LogLevel::kWarn:
            return 3;
        case LogLevel::kError:
            return 4;
        case LogLevel::kCritical:
            return 5;
        case LogLevel::kOff:
            return 6;
    }
    return 2;
}

LogLevel ToLogLevel(const Severity severity) {
    switch (severity) {
        case Severity::kTrace:
        case Severity::kTrace2:
        case Severity::kTrace3:
        case Severity::kTrace4:
            return LogLevel::kTrace;
        case Severity::kDebug:
        case Severity::kDebug2:
        case Severity::kDebug3:
        case Severity::kDebug4:
            return LogLevel::kDebug;
        case Severity::kInfo:
        case Severity::kInfo2:
        case Severity::kInfo3:
        case Severity::kInfo4:
            return LogLevel::kInfo;
        case Severity::kWarn:
        case Severity::kWarn2:
        case Severity::kWarn3:
        case Severity::kWarn4:
            return LogLevel::kWarn;
        case Severity::kError:
        case Severity::kError2:
        case Severity::kError3:
        case Severity::kError4:
            return LogLevel::kError;
        case Severity::kFatal:
        case Severity::kFatal2:
        case Severity::kFatal3:
        case Severity::kFatal4:
            return LogLevel::kCritical;
        case Severity::kInvalid:
            return LogLevel::kDebug;
    }
    return LogLevel::kDebug;
}

bool ShouldLogLevel(const LogLevel level) {
    const LogLevel current_level = g_log_level.load(std::memory_order_relaxed);
    if (current_level == LogLevel::kOff) {
        return false;
    }
    return LogLevelRank(level) >= LogLevelRank(current_level);
}

bool ShouldLogSeverity(const Severity severity) {
    return ShouldLogLevel(ToLogLevel(severity));
}

void WriteLineToStderr(const std::string_view line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

std::string AttributeToString(const opentelemetry::common::AttributeValue& value) {
    constexpr size_t kMaxAttributeArrayItems = 16;
    auto format_span = [](const auto span, const auto& append_value) {
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
                return format_span(v, [](std::string& out, const bool item) {
                    out.append(item ? "true" : "false");
                });
            } else if constexpr (std::is_same_v<ValueType, SpanInt32> ||
                                 std::is_same_v<ValueType, SpanInt64> ||
                                 std::is_same_v<ValueType, SpanUInt32> ||
                                 std::is_same_v<ValueType, SpanDouble> ||
                                 std::is_same_v<ValueType, SpanUInt64>) {
                return format_span(v, [](std::string& out, const auto item) {
                    out.append(fmt::format("{}", item));
                });
            } else if constexpr (std::is_same_v<ValueType, SpanStringView>) {
                return format_span(
                    v,
                    [](std::string& out, const opentelemetry::nostd::string_view item) {
                        out.push_back('"');
                        out.append(item.data(), item.size());
                        out.push_back('"');
                    }
                );
            } else if constexpr (std::is_same_v<ValueType, SpanUInt8>) {
                return format_span(v, [](std::string& out, const uint8_t item) {
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
    span_id.ToLowerBase16(opentelemetry::nostd::span<char, opentelemetry::trace::SpanId::kSize * 2>(
        buffer.data(), buffer.size()
    ));
    return std::string(buffer.data(), buffer.size());
}

std::string TraceFlagsToHex(const opentelemetry::trace::TraceFlags& flags) {
    std::array<char, 2> buffer{};
    flags.ToLowerBase16(opentelemetry::nostd::span<char, 2>(buffer.data(), buffer.size()));
    return std::string(buffer.data(), buffer.size());
}

std::string RenderMessage(
    const std::string& body, const std::vector<std::pair<std::string, std::string>>& attributes
) {
    std::string_view filepath;
    std::string_view line;
    for (const auto& [key, value] : attributes) {
        if (key == "code.filepath") {
            filepath = value;
            continue;
        }
        if (key == "code.lineno") {
            line = value;
        }
    }

    std::string message;
    if (const std::string location = FormatCodeLocation(filepath, line); !location.empty()) {
        message.append(location);
        if (!body.empty()) {
            message.push_back(' ');
        }
    }

    message.append(body);
    for (const auto& [key, value] : attributes) {
        if (key == "code.filepath" || key == "code.lineno") {
            continue;
        }
        if (!message.empty()) {
            message.push_back(' ');
        }
        message.append(key);
        message.push_back('=');
        message.append(value);
    }
    return message;
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

class StderrLogRecord final : public opentelemetry::logs::LogRecord {
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

    void SetSeverity(const opentelemetry::logs::Severity severity) noexcept override {
        severity_ = severity;
    }

    void SetBody(const opentelemetry::common::AttributeValue& message) noexcept override {
        body_ = AttributeToString(message);
    }

    void SetAttribute(
        const opentelemetry::nostd::string_view key,
        const opentelemetry::common::AttributeValue& value
    ) noexcept override {
        AddAttribute(std::string(key.data(), key.size()), AttributeToString(value));
    }

    void SetEventId(
        const int64_t id, const opentelemetry::nostd::string_view name
    ) noexcept override {
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

    [[nodiscard]] Severity severity() const { return severity_; }

    [[nodiscard]] const std::string& body() const { return body_; }

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& attributes() const {
        return attributes_;
    }

  private:
    void AddAttribute(std::string key, std::string value) {
        attributes_.emplace_back(std::move(key), std::move(value));
    }

    Severity severity_ = Severity::kInfo;
    std::string body_;
    std::vector<std::pair<std::string, std::string>> attributes_;
};

class StderrLogger final : public opentelemetry::logs::Logger {
  public:
    explicit StderrLogger(std::string name) : name_(std::move(name)) {}

    const opentelemetry::nostd::string_view GetName() noexcept override { return name_; }

    opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord> CreateLogRecord(
    ) noexcept override {
        return opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>(new StderrLogRecord
        );
    }

    using Logger::EmitLogRecord;

    void EmitLogRecord(opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>&& record
    ) noexcept override {
        auto* stderr_record = dynamic_cast<StderrLogRecord*>(record.get());
        if (stderr_record == nullptr || !ShouldLogSeverity(stderr_record->severity())) {
            return;
        }
        WriteLineToStderr(RenderMessage(stderr_record->body(), stderr_record->attributes()));
    }

  private:
    std::string name_;
};

class StderrLoggerProvider final : public opentelemetry::logs::LoggerProvider {
  public:
    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> GetLogger(
        const opentelemetry::nostd::string_view logger_name,
        opentelemetry::nostd::string_view /*library_name*/,
        opentelemetry::nostd::string_view /*library_version*/,
        opentelemetry::nostd::string_view /*schema_url*/,
        const opentelemetry::common::KeyValueIterable& /*attributes*/
    ) override {
        return opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger>(
            new StderrLogger(std::string(logger_name.data(), logger_name.size()))
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
                    new StderrLoggerProvider()
                )
            );
        }
    });
}

}  // namespace

void SetLogLevel(const LogLevel level) {
    g_log_level.store(level, std::memory_order_relaxed);
}

void ConfigureFromEnv(const LogLevel default_level) {
    SetLogLevel(default_level);
    const char* value = std::getenv("RAFTPP_LOG_LEVEL");
    if (value == nullptr) {
        return;
    }
    if (const std::optional<LogLevel> parsed = ParseLogLevel(value); parsed.has_value()) {
        SetLogLevel(*parsed);
    }
}

bool ShouldLog(const Severity severity) {
    EnsureProviderInstalled();
    auto provider = opentelemetry::logs::Provider::GetLoggerProvider();
    if (dynamic_cast<opentelemetry::logs::NoopLoggerProvider*>(provider.get()) != nullptr) {
        return false;
    }
    if (dynamic_cast<StderrLoggerProvider*>(provider.get()) == nullptr) {
        return true;
    }
    return ShouldLogSeverity(severity);
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
