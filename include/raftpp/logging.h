#pragma once

#include <absl/log/log_sink.h>

namespace raftpp {

class LogSink final : public absl::LogSink {
public:
    void Send(const absl::LogEntry& entry) override;
};

void RegisterLogSink();

}
