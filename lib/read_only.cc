#include "raftpp/read_only.h"

#include <libassert/assert.hpp>

#include "raftpp/util.h"

namespace raftpp {

ReadOnly::ReadOnly(const ReadOnlyOption option) : option_(option) {}

std::optional<std::string> ReadOnly::LastPendingRequestCtx() const {
    return read_index_queue_.back();
}

size_t ReadOnly::PendingReadCount() const {
    return read_index_queue_.size();
}

std::optional<Set<uint64_t>> ReadOnly::RecvACK(const uint64_t id, const std::string& ctx) {
    if (const auto it = pending_read_index_.find(ctx); it != pending_read_index_.end()) {
        it->second.acks.insert(id);
        return it->second.acks;
    }
    return {};
}

ReadOnlyOption ReadOnly::option() const {
    return option_;
}

std::vector<ReadIndexStatus> ReadOnly::Advance(const std::string& ctx) {
    std::vector<ReadIndexStatus> rss;

    size_t p = -1;
    for (size_t i = 0; i < read_index_queue_.size(); ++i) {
        const auto& x = read_index_queue_[i];
        if (!pending_read_index_.contains(x)) {
            PANIC("cannot find correspond read state from pending map");
        }
        if (x == ctx) {
            p = i;
            break;
        }
    }

    if (p != -1) {
        for (size_t i = 0; i <= p; ++i) {
            const auto rs = read_index_queue_.front();
            read_index_queue_.pop_front();

            const auto it = pending_read_index_.find(rs);
            ASSERT(it != pending_read_index_.end());
            rss.emplace_back(it->second);
            pending_read_index_.erase(it);
        }
    }

    return rss;
}

}  // namespace raftpp
