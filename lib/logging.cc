#include "raftpp/logging.h"

#include <absl/log/initialize.h>
#include <absl/log/internal/globals.h>
#include <absl/log/log_sink_registry.h>
#include <spdlog/spdlog.h>

namespace raftpp {

void LogSink::Send(const absl::LogEntry& entry) {
    switch (entry.log_severity()) {
        case absl::LogSeverity::kInfo:
        case absl::LogSeverity::kWarning:
        case absl::LogSeverity::kError:
            break;
        case absl::LogSeverity::kFatal:
            spdlog::critical("{}", entry.text_message());
            break;
    }
}

void RegisterLogSink() {
    // absl::InitializeLog();
    static LogSink sink;
    absl::AddLogSink(&sink);
}

}  // namespace raftpp
