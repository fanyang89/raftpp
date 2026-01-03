#pragma once

#include <optional>

#include "storage.h"

namespace raftpp {

class MemoryStorageCore {
  public:
    MemoryStorageCore();

    void SetHardState(HardState&& hs);
    void CommitTo(uint64_t index);
    bool HasEntryAt(uint64_t index) const;
    Result<void> ApplySnapshot(const Snapshot& snapshot);
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
    ~MemoryStorage() override;
    Result<RaftState> InitialState() override;

    Result<std::vector<Entry>> Entries(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size, GetEntriesContext context
    ) override;
    void SetEntries(const std::vector<Entry>& entries);

    Result<uint64_t> Term(uint64_t idx) override;
    Result<uint64_t> FirstIndex() override;
    Result<uint64_t> LastIndex() override;
    Result<Snapshot> GetSnapshot(uint64_t request_index, uint64_t to) override;

  private:
    std::mutex mutex_;
    MemoryStorageCore core_;
};

}  // namespace raftpp
