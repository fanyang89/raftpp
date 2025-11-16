#include "raftpp/progress.h"

#include <libassert/assert.hpp>

#include "raftpp/primitives.h"

namespace raftpp {

Inflights::Inflights(const size_t capacity) : buffer_(capacity) {}

void Inflights::Reset() {
    buffer_.clear();
}

void Inflights::Add(const uint64_t last) {
    buffer_.push_back(last);
}

bool Inflights::Full() const {
    return buffer_.full();
}

Progress::Progress(const uint64_t next_idx)
    : matched_(0),
      next_idx_(next_idx),
      state_(ProgressState::Probe),
      paused_(false),
      pending_snapshot_(0),
      pending_request_snapshot_(0),
      recent_active_(false),
      commit_group_id_(0),
      committed_index_(0) {}

void Progress::Reset(const uint64_t next_idx) {
    matched_ = 0;
    next_idx_ = next_idx;
    state_ = ProgressState::Probe;
    paused_ = false;
    pending_snapshot_ = 0;
    pending_request_snapshot_ = 0;
    recent_active_ = false;
    inflights_.Reset();
}

void Progress::BecomeProbe() {
    if (state_ == ProgressState::Snapshot) {
        const uint64_t pending_snapshot = pending_snapshot_;
        ResetState(ProgressState::Probe);
        next_idx_ = std::max(matched_ + 1, pending_snapshot + 1);
    } else {
        ResetState(ProgressState::Probe);
        next_idx_ = matched_ + 1;
    }
}

void Progress::BecomeReplicate() {
    ResetState(ProgressState::Replicate);
    next_idx_ = matched_ + 1;
}

void Progress::BecomeSnapshot(const uint64_t snapshot_idx) {
    ResetState(ProgressState::Snapshot);
    pending_snapshot_ = snapshot_idx;
}

bool Progress::IsSnapshotCaughtUp() const {
    return state_ == ProgressState::Snapshot && matched_ >= pending_snapshot_;
}

void Progress::Resume() {
    paused_ = false;
}

void Progress::Pause() {
    paused_ = true;
}

bool Progress::MaybeUpdate(const uint64_t n) {
    const bool need_update = matched_ < n;
    if (need_update) {
        matched_ = n;
        Resume();
    }

    if (next_idx_ < n + 1) {
        next_idx_ = n + 1;
    }

    return need_update;
}

void Progress::UpdateState(const uint64_t last) {
    switch (state_) {
        case ProgressState::Probe:
            Pause();
            break;
        case ProgressState::Replicate:
            OptimisticUpdate(last);
            inflights_.Add(last);
            break;
        case ProgressState::Snapshot:
            PANIC("updating progress state in unhandled state {}", state_);
    }
}

void Progress::OptimisticUpdate(const uint64_t n) {
    next_idx_ = n + 1;
}

bool Progress::IsPaused() const {
    switch (state_) {
        case ProgressState::Probe:
            return paused_;
        case ProgressState::Replicate:
            return inflights_.Full();
        case ProgressState::Snapshot:
            return true;
        default:
            PANIC("Progress::IsPaused()");
    }
}

bool Progress::MaybeDecTo(const uint64_t rejected, uint64_t match_hint, const uint64_t request_snapshot) {
    if (state_ == ProgressState::Replicate) {
        if (rejected < matched_ || (rejected == matched_ && request_snapshot == INVALID_INDEX)) {
            return false;
        }

        if (request_snapshot == INVALID_INDEX) {
            next_idx_ = matched_ + 1;
        } else {
            pending_request_snapshot_ = request_snapshot;
        }
        return true;
    }

    if ((next_idx_ == 0 || next_idx_ - 1 != rejected)
        && request_snapshot == INVALID_INDEX) {
        return false;
    }

    if (request_snapshot == INVALID_INDEX) {
        next_idx_ = std::min(rejected, match_hint + 1);
        if (next_idx_ < matched_ + 1) {
            next_idx_ = matched_ + 1;
        }
    } else if (pending_request_snapshot_ == INVALID_INDEX) {
        pending_request_snapshot_ = request_snapshot;
    }

    Resume();
    return true;
}

uint64_t Progress::Matched() const {
    return matched_;
}

uint64_t Progress::CommitGroupID() const {
    return commit_group_id_;
}

void Progress::ResetState(const ProgressState state) {
    paused_ = false;
    pending_snapshot_ = 0;
    state_ = state;
    inflights_.Reset();
}

ProgressState& ProgressDebug::state() {
    return state_;
}

uint64_t& ProgressDebug::matched() {
    return matched_;
}

uint64_t& ProgressDebug::pending_snapshot() {
    return pending_snapshot_;
}

bool& ProgressDebug::paused() {
    return paused_;
}

std::optional<Index> ProgressMap::AckedIndex(const uint64_t voter) {
    if (const auto& it = find(voter); it != end()) {
        const auto& [_, p] = *it;
        return Index{p.Matched(), p.CommitGroupID()};
    }
    return {};
}

}
