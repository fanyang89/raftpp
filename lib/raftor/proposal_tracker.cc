#include "raftpp/raftor/proposal_tracker.h"

namespace raftpp::raftor {

// === ProposalTracker ===

void ProposalTracker::Track(const std::string& ctx, ProposalCallback callback) {
    std::lock_guard lock(mutex_);
    proposals_[ctx] = std::move(callback);
}

void ProposalTracker::Complete(const std::string& ctx, const std::string& response) {
    ProposalCallback callback;
    {
        std::lock_guard lock(mutex_);
        auto it = proposals_.find(ctx);
        if (it == proposals_.end()) {
            return;  // Already completed or not tracked
        }
        callback = std::move(it->second);
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
        callback = std::move(it->second);
        proposals_.erase(it);
    }
    if (callback) {
        callback(std::unexpected(error));
    }
}

void ProposalTracker::FailAll(RaftError error) {
    absl::flat_hash_map<std::string, ProposalCallback> callbacks;
    {
        std::lock_guard lock(mutex_);
        callbacks = std::move(proposals_);
        proposals_.clear();
    }
    for (auto& [ctx, callback] : callbacks) {
        if (callback) {
            callback(std::unexpected(error));
        }
    }
}

void ProposalTracker::TrackRead(const std::string& ctx, ReadIndexCallback callback) {
    std::lock_guard lock(mutex_);
    reads_[ctx] = std::move(callback);
}

void ProposalTracker::CompleteRead(const std::string& ctx) {
    ReadIndexCallback callback;
    {
        std::lock_guard lock(mutex_);
        auto it = reads_.find(ctx);
        if (it == reads_.end()) {
            return;
        }
        callback = std::move(it->second);
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
        callback = std::move(it->second);
        reads_.erase(it);
    }
    if (callback) {
        callback(std::unexpected(error));
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

// === ProposalQueue ===

void ProposalQueue::Push(std::string data, ProposalCallback callback) {
    std::lock_guard lock(mutex_);
    queue_.emplace_back(std::move(data), std::move(callback));
}

std::optional<std::pair<std::string, ProposalCallback>> ProposalQueue::TryPop() {
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
    queue_.emplace_back(std::move(ctx), std::move(callback));
}

std::optional<std::pair<std::string, ReadIndexCallback>> ReadIndexQueue::TryPop() {
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
