#pragma once

#include <string_view>

#include <magic_enum/magic_enum.hpp>
#include <boost/circular_buffer.hpp>

#include "raftpp/quorum.h"
#include "raftpp/primitives.h"

namespace raftpp {

class Inflights {
public:
    explicit Inflights(size_t capacity = 256);

    void Add(uint64_t last);
    bool Full() const;
    void Reset();

private:
    boost::circular_buffer<uint64_t> buffer_;
};

enum class ProgressState : uint8_t {
    /// Whether it's probing.
    Probe,
    /// Whether it's replicating.
    Replicate,
    /// Whether it's a snapshot.
    Snapshot,
};

constexpr std::string_view format_as(const ProgressState c) {
    return magic_enum::enum_name(c);
}

class ProgressDebug;

class Progress {
public:
    explicit Progress(uint64_t next_idx);

    void Reset(uint64_t next_idx);

    void BecomeProbe();
    void BecomeReplicate();
    void BecomeSnapshot(uint64_t snapshot_idx);

    bool IsSnapshotCaughtUp() const;
    void Resume();
    void Pause();
    bool MaybeUpdate(uint64_t n);
    void UpdateState(uint64_t last);
    void OptimisticUpdate(uint64_t n);
    bool IsPaused() const;
    bool MaybeDecTo(uint64_t rejected, uint64_t match_hint, uint64_t request_snapshot);

    uint64_t Matched() const;
    uint64_t CommitGroupID() const;

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

class ProgressDebug : public Progress {
public:
    using Progress::Progress;

    ProgressState& state();
    uint64_t& matched();
    uint64_t& pending_snapshot();
    bool& paused();
};

class ProgressMap final : public Map<uint64_t, Progress>, public AckedIndexer {
public:
    std::optional<Index> AckedIndex(uint64_t voter) override;
};

}
