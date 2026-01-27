#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/span_metadata.h>
#include <opentelemetry/trace/tracer.h>

namespace raftpp::raftor::telemetry {

inline opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> GetTracer() {
    static auto tracer =
        opentelemetry::trace::Provider::GetTracerProvider()->GetTracer("raftpp.raftor", "0.1.0");
    return tracer;
}

inline void SetNodeId(
    const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>& span, uint64_t node_id
) {
    if (!span) {
        return;
    }
    span->SetAttribute("raft.node_id", static_cast<int64_t>(node_id));
}

inline opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> StartSpanWithNodeId(
    const char* name, uint64_t node_id
) {
    auto span = GetTracer()->StartSpan(name);
    SetNodeId(span, node_id);
    return span;
}

class ScopedSpan {
  public:
    explicit ScopedSpan(const char* name) : span_(GetTracer()->StartSpan(name)) {
        scope_.emplace(span_);
    }

    ScopedSpan(const char* name, uint64_t node_id) : span_(GetTracer()->StartSpan(name)) {
        SetNodeId(span_, node_id);
        scope_.emplace(span_);
    }

    ~ScopedSpan() {
        if (span_) {
            span_->End();
        }
    }

    [[nodiscard]] const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>& span() const {
        return span_;
    }

  private:
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
    std::optional<opentelemetry::trace::Scope> scope_;
};

inline void RecordError(
    const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>& span,
    std::string_view message
) {
    if (!span) {
        return;
    }
    span->SetStatus(opentelemetry::trace::StatusCode::kError, message.data());
    span->SetAttribute("error", true);
    span->SetAttribute("error.message", std::string(message));
}

template <typename ResultT>
inline bool RecordErrorIf(
    const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>& span, const ResultT& result
) {
    if (result) {
        return false;
    }
    RecordError(span, result.error().ToString());
    return true;
}

}  // namespace raftpp::raftor::telemetry
