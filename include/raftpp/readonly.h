#pragma once

#include <deque>
#include <vector>

#include "raftpp/primitives.h"
#include "raftpp/raftpp.pb.h"

namespace raftpp {

enum class ReadOnlyOption {
    Safe,
    LeaseBased
};

struct ReadState {
    uint64_t index;
    std::vector<uint8_t> request_ctx;
};

struct ReadIndexStatus {
    Message req;
    uint64_t index;
    Set<uint64_t> acks;
};

class ReadOnly {
public:
    explicit ReadOnly(ReadOnlyOption option);

private:
    ReadOnlyOption option_;
    Map<std::vector<uint8_t>, ReadIndexStatus> pending_read_index_;
    std::deque<std::vector<uint8_t>> read_index_queue_;
};

}
