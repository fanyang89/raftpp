#pragma once

#include <expected>
#include <mutex>

#include <absl/base/thread_annotations.h>

#include "raftpp/error.h"
#include "raftpp/raftpp.pb.h"

namespace raftpp {

struct RaftState {
    HardState hard_state;
    ConfState conf_state;
};

enum class GetEntriesFor {
    // for sending entries to followers
    SendAppend,
    // for getting committed entries in a ready
    GenReady,
    // for getting entries to check pending conf when transferring leader
    TransferLeader,
    // for getting entries to check pending conf when forwarding commit index by vote messages
    CommitByVote,
    // It's not called by the raft itself
    Empty,
};

union GetEntriesForPayload {
    struct Empty {
        bool can_async;
    };

    Empty empty;

    struct SendAppend {
        /// the peer id to which the entries are going to send
        uint64_t to;
        /// the term when the request is issued
        uint64_t term;
        /// whether to exhaust all the entries
        bool aggressively;
    };

    SendAppend send_append;
};

struct GetEntriesContext {
    GetEntriesFor what;
    GetEntriesForPayload payload;

    bool CanAsync() const;

    static GetEntriesContext Empty(bool can_async);
};

class Storage {
  public:
    virtual ~Storage();
    virtual Result<RaftState, StorageErrorCode> InitialState() = 0;

    virtual Result<std::vector<Entry>, StorageErrorCode> Entries(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size, GetEntriesContext context
    ) = 0;

    virtual Result<uint64_t, StorageErrorCode> Term(uint64_t idx) = 0;
    virtual Result<uint64_t, StorageErrorCode> FirstIndex() = 0;
    virtual Result<uint64_t, StorageErrorCode> LastIndex() = 0;
    virtual Result<Snapshot, StorageErrorCode> GetSnapshot(uint64_t request_index, uint64_t to) = 0;
};

class MemoryStorageCore {
  public:
    MemoryStorageCore();

    void SetHardState(HardState hs);
    void CommitTo(uint64_t index);
    bool HasEntryAt(uint64_t index) const;
    Result<void, StorageErrorCode> ApplySnapshot(const Snapshot& snapshot);
    void Compact(uint64_t compact_index);
    void Append(const std::vector<Entry>& ents);
    void TriggerSnapshotUnavailable();
    void TriggerLogUnavailable();
    std::optional<GetEntriesContext> TakeGetEntriesContext();

    friend class MemoryStorage;

  private:
    uint64_t first_index() const;
    uint64_t last_index() const;
    Snapshot snapshot() const;

    RaftState raft_state_;
    std::vector<Entry> entries_;
    SnapshotMetadata snapshot_metadata_;
    bool trigger_snapshot_unavailable_;
    bool trigger_log_unavailable_;
    std::optional<GetEntriesContext> get_entries_context_;
};

class MemoryStorage final : public Storage {
  public:
    MemoryStorage();

    Result<RaftState, StorageErrorCode> InitialState() override;

    Result<std::vector<Entry>, StorageErrorCode> Entries(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size, GetEntriesContext context
    ) override;

    Result<uint64_t, StorageErrorCode> Term(uint64_t idx) override;
    Result<uint64_t, StorageErrorCode> FirstIndex() override;
    Result<uint64_t, StorageErrorCode> LastIndex() override;
    Result<Snapshot, StorageErrorCode> GetSnapshot(uint64_t request_index, uint64_t to) override;

  private:
    std::mutex mutex_;
    MemoryStorageCore core_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace raftpp
