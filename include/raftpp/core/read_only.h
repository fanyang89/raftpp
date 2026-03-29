#pragma once

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "primitives.h"
#include "types.h"

namespace raftpp {

enum class ReadOnlyOption { Safe, LeaseBased };

struct ReadState {
    uint64_t index;
    std::string request_ctx;
};

struct ReadIndexStatus {
    Message req;
    uint64_t index;
    Set<uint64_t> acks;
};

class ReadOnly {
  public:
    explicit ReadOnly(ReadOnlyOption option);

    void AddRequest(uint64_t index, const Message& req, uint64_t self_id);
    [[nodiscard]] std::optional<std::string> LastPendingRequestCtx() const;
    [[nodiscard]] size_t PendingReadCount() const;
    [[nodiscard]] std::optional<Set<uint64_t>> RecvACK(uint64_t id, const std::string& ctx);
    [[nodiscard]] std::vector<ReadIndexStatus> Advance(const std::string& ctx);

    [[nodiscard]] ReadOnlyOption option() const;

  private:
    ReadOnlyOption option_;
    Map<std::string, ReadIndexStatus> pending_read_index_;
    std::deque<std::string> read_index_queue_;
};

}  // namespace raftpp
