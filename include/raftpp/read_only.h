#pragma once

#include <deque>
#include <vector>

#include "raftpp/primitives.h"
#include "raftpp/raftpp.pb.h"

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

    std::optional<std::string> LastPendingRequestCtx() const;
    size_t PendingReadCount() const;
    std::optional<Set<uint64_t>> RecvACK(uint64_t id, const std::string& ctx);
    std::vector<ReadIndexStatus> Advance(const std::string& ctx);

    ReadOnlyOption option() const;

  private:
    ReadOnlyOption option_;
    Map<std::string, ReadIndexStatus> pending_read_index_;
    std::deque<std::string> read_index_queue_;
};

}  // namespace raftpp
