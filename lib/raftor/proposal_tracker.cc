#include "raftpp/raftor/proposal_tracker.h"

#include <vector>

namespace raftpp::raftor {

// === ProposalTracker ===

namespace {

std::chrono::steady_clock::time_point DeadlineFromTimeout(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        return std::chrono::steady_clock::time_point::max();
    }
    return std::chrono::steady_clock::now() + timeout;
}

template <typename Map>
void FailAllPending(Map& pending, std::mutex& mutex, const RaftError& error) {
    Map callbacks;
    {
        std::lock_guard lock(mutex);
        callbacks = std::move(pending);
        pending.clear();
    }
    for (auto& [ctx, entry] : callbacks) {
        if (entry.callback) {
            entry.callback(std::unexpected(error));
        }
    }
}

}  // namespace

void ProposalTracker::Track(
    const std::string& ctx, ProposalCallback callback, std::chrono::milliseconds timeout
) {
    std::lock_guard lock(mutex_);
    proposals_[ctx] = PendingProposal{std::move(callback), DeadlineFromTimeout(timeout)};
}

void ProposalTracker::Complete(const std::string& ctx, const std::string& response) {
    ProposalCallback callback;
    {
        std::lock_guard lock(mutex_);
        auto it = proposals_.find(ctx);
        if (it == proposals_.end()) {
            return;  // Already completed or not tracked
        }
        callback = std::move(it->second.callback);
        proposals_.erase(it);
    }
    if (callback) {
        callback(response);
    }
}

void ProposalTracker::Fail(const std::string& ctx, RaftError error) {
    ProposalCallback callback;
    {
        std::lock_guard lock(mutex_);
        auto it = proposals_.find(ctx);
        if (it == proposals_.end()) {
            return;  // Already completed or not tracked
        }
        callback = std::move(it->second.callback);
        proposals_.erase(it);
    }
    if (callback) {
        callback(std::unexpected(std::move(error)));
    }
}

void ProposalTracker::FailAll(RaftError error) {
    FailAllPending(proposals_, mutex_, error);
}

void ProposalTracker::FailAllReads(RaftError error) {
    FailAllPending(reads_, mutex_, error);
}

void ProposalTracker::TrackRead(
    const std::string& ctx, ReadIndexCallback callback, std::chrono::milliseconds timeout
) {
    std::lock_guard lock(mutex_);
    reads_[ctx] = PendingRead{std::move(callback), DeadlineFromTimeout(timeout)};
}

void ProposalTracker::CompleteRead(const std::string& ctx) {
    ReadIndexCallback callback;
    {
        std::lock_guard lock(mutex_);
        auto it = reads_.find(ctx);
        if (it == reads_.end()) {
            return;
        }
        callback = std::move(it->second.callback);
        reads_.erase(it);
    }
    if (callback) {
        callback({});
    }
}

void ProposalTracker::FailRead(const std::string& ctx, RaftError error) {
    ReadIndexCallback callback;
    {
        std::lock_guard lock(mutex_);
        auto it = reads_.find(ctx);
        if (it == reads_.end()) {
            return;
        }
        callback = std::move(it->second.callback);
        reads_.erase(it);
    }
    if (callback) {
        callback(std::unexpected(std::move(error)));
    }
}

size_t ProposalTracker::PendingCount() const {
    std::lock_guard lock(mutex_);
    return proposals_.size();
}

size_t ProposalTracker::PendingReadCount() const {
    std::lock_guard lock(mutex_);
    return reads_.size();
}

void ProposalTracker::ExpireTimeouts(std::chrono::steady_clock::time_point now) {
    std::vector<ProposalCallback> proposal_callbacks;
    std::vector<ReadIndexCallback> read_callbacks;

    auto collect_expired = [&](auto& pending, auto& callbacks) {
        absl::erase_if(pending, [&](auto& entry) {
            if (entry.second.deadline > now) {
                return false;
            }
            callbacks.push_back(std::move(entry.second.callback));
            return true;
        });
    };

    {
        std::lock_guard lock(mutex_);
        collect_expired(proposals_, proposal_callbacks);
        collect_expired(reads_, read_callbacks);
    }

    if (proposal_callbacks.empty() && read_callbacks.empty()) {
        return;
    }

    for (auto& callback : proposal_callbacks) {
        if (callback) {
            callback(std::unexpected(RaftError(RpcErrorCode::Timeout)));
        }
    }

    for (auto& callback : read_callbacks) {
        if (callback) {
            callback(std::unexpected(RaftError(RpcErrorCode::Timeout)));
        }
    }
}

// === ProposalQueue ===

void ProposalQueue::Push(std::string data, ProposalCallback callback) {
    std::lock_guard lock(mutex_);
    queue_.push_back(
        ProposalQueueItem{
            .data = std::move(data),
            .callback = std::move(callback),
            .timeout = std::nullopt,
        }
    );
}

void ProposalQueue::Push(
    std::string data, ProposalCallback callback, std::chrono::milliseconds timeout
) {
    std::lock_guard lock(mutex_);
    queue_.push_back(
        ProposalQueueItem{
            .data = std::move(data),
            .callback = std::move(callback),
            .timeout = timeout,
        }
    );
}

std::optional<std::pair<std::string, ProposalCallback>> ProposalQueue::TryPop() {
    auto item = TryPopWithTimeout();
    if (!item) {
        return std::nullopt;
    }
    return std::make_pair(std::move(item->data), std::move(item->callback));
}

std::optional<ProposalQueue::ProposalQueueItem> ProposalQueue::TryPopWithTimeout() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
        return std::nullopt;
    }
    auto item = std::move(queue_.front());
    queue_.pop_front();
    return item;
}

bool ProposalQueue::Empty() const {
    std::lock_guard lock(mutex_);
    return queue_.empty();
}

size_t ProposalQueue::Size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

// === ReadIndexQueue ===

void ReadIndexQueue::Push(std::string ctx, ReadIndexCallback callback) {
    std::lock_guard lock(mutex_);
    queue_.push_back(
        ReadIndexQueueItem{
            .ctx = std::move(ctx),
            .callback = std::move(callback),
            .timeout = std::nullopt,
        }
    );
}

void ReadIndexQueue::Push(
    std::string ctx, ReadIndexCallback callback, std::chrono::milliseconds timeout
) {
    std::lock_guard lock(mutex_);
    queue_.push_back(
        ReadIndexQueueItem{
            .ctx = std::move(ctx),
            .callback = std::move(callback),
            .timeout = timeout,
        }
    );
}

std::optional<std::pair<std::string, ReadIndexCallback>> ReadIndexQueue::TryPop() {
    auto item = TryPopWithTimeout();
    if (!item) {
        return std::nullopt;
    }
    return std::make_pair(std::move(item->ctx), std::move(item->callback));
}

std::optional<ReadIndexQueue::ReadIndexQueueItem> ReadIndexQueue::TryPopWithTimeout() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
        return std::nullopt;
    }
    auto item = std::move(queue_.front());
    queue_.pop_front();
    return item;
}

bool ReadIndexQueue::Empty() const {
    std::lock_guard lock(mutex_);
    return queue_.empty();
}

}  // namespace raftpp::raftor
