#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ack_indexer.h"
#include "inflights.h"
#include "primitives.h"

namespace raftpp {

enum class ProgressState : uint8_t {
    /// Whether it's probing.
    Probe,
    /// Whether it's replicating.
    Replicate,
    /// Whether it's a snapshot.
    Snapshot,
};

constexpr std::string_view format_as(ProgressState c) {
    switch (c) {
        case ProgressState::Probe:
            return "Probe";
        case ProgressState::Replicate:
            return "Replicate";
        case ProgressState::Snapshot:
            return "Snapshot";
    }
    return "Unknown";
}

class Progress {
  public:
    explicit Progress(uint64_t next_idx, size_t max_inflight = 256);

    [[nodiscard]] bool IsPaused() const;
    [[nodiscard]] bool IsSnapshotCaughtUp() const;
    [[nodiscard]] bool MaybeDecTo(
        uint64_t rejected, uint64_t match_hint, uint64_t request_snapshot
    );
    [[nodiscard]] bool MaybeUpdate(uint64_t n);
    [[nodiscard]] uint64_t CommitGroupID() const;
    void SetCommitGroupID(uint64_t group_id);
    [[nodiscard]] uint64_t Matched() const;
    void BecomeProbe();
    void BecomeReplicate();
    void BecomeSnapshot(uint64_t snapshot_idx);
    void OptimisticUpdate(uint64_t n);
    void Pause();
    void Reset(uint64_t next_idx);
    void Resume();
    void UpdateCommitted(uint64_t committed_index);
    void UpdateState(uint64_t last);
    void SnapshotFailure();

    [[nodiscard]] uint64_t committed_index() const;
    uint64_t& committed_index();
    [[nodiscard]] uint64_t matched() const;
    uint64_t& matched();
    [[nodiscard]] bool recent_active() const;
    bool& recent_active();
    [[nodiscard]] uint64_t pending_snapshot() const;
    uint64_t& pending_snapshot();
    [[nodiscard]] uint64_t pending_request_snapshot() const;
    uint64_t& pending_request_snapshot();
    [[nodiscard]] uint64_t next_idx() const;
    uint64_t& next_idx();
    [[nodiscard]] Inflights& inflights();
    [[nodiscard]] const Inflights& inflights() const;
    [[nodiscard]] ProgressState state() const;

  protected:
    friend class ProgressDebug;

  private:
    void ResetState(ProgressState state);
    uint64_t matched_;
    uint64_t next_idx_;
    ProgressState state_;
    bool paused_;
    uint64_t pending_snapshot_;
    uint64_t pending_request_snapshot_;
    bool recent_active_;
    Inflights inflights_;
    uint64_t commit_group_id_;
    uint64_t committed_index_;
};

class ProgressMap final : public Map<uint64_t, Progress>, public AckedIndexer {
  public:
    [[nodiscard]] std::optional<Index> AckedIndex(uint64_t voter) const override;
};

class ProgressDebug : public Progress {
  public:
    using Progress::Progress;

    ProgressState& state();
    uint64_t& matched();
    uint64_t& pending_snapshot();
    bool& paused();
};

}  // namespace raftpp
