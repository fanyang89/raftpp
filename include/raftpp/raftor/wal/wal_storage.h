#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "raftpp/core/storage.h"
#include "raftpp/raftor/wal/wal.h"
#include "raftpp/raftor/wal/wal_config.h"

namespace raftpp::raftor::wal {

// WALStorage implements the Storage interface using a Write-Ahead Log
// This provides durable storage for Raft log entries
class WALStorage final : public Storage {
  public:
    ~WALStorage() override;

    // Factory method to create a WALStorage
    [[nodiscard]] static Result<std::shared_ptr<WALStorage>> Open(const WALConfig& config);

    // Storage interface implementation
    [[nodiscard]] Result<RaftState> InitialState() override;

    [[nodiscard]] Result<std::vector<Entry>> Entries(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size, GetEntriesContext context
    ) override;

    [[nodiscard]] Result<uint64_t> Term(uint64_t idx) override;

    [[nodiscard]] Result<uint64_t> FirstIndex() override;

    [[nodiscard]] Result<uint64_t> LastIndex() override;

    [[nodiscard]] Result<Snapshot> GetSnapshot(uint64_t request_index, uint64_t to) override;

    // Mutation methods (following MemoryStorage pattern)

    // Set the hard state
    void SetHardState(HardState&& hs);

    // Append entries to the log
    [[nodiscard]] Result<void> Append(const std::vector<Entry>& entries);

    // Compact the log by removing entries before compact_index
    [[nodiscard]] Result<void> Compact(uint64_t compact_index);

    // Apply a snapshot
    [[nodiscard]] Result<void> ApplySnapshot(const Snapshot& snapshot);

    // Set the conf state
    void SetConfState(const ConfState& conf_state);

    // Sync all pending writes to disk
    [[nodiscard]] Result<void> Sync();

    // Get approximate WAL size in bytes
    [[nodiscard]] uint64_t LogSizeBytes() const;

    // Get all entries (for testing)
    [[nodiscard]] std::vector<Entry> AllEntries();

    // Check if storage has been initialized with a cluster configuration.
    // @return true if ConfState contains at least one voter.
    [[nodiscard]] bool IsInitialized() const;

    // Get the effective IO backend selected for this WAL instance.
    [[nodiscard]] WALIoBackend EffectiveIoBackend() const;

    // Human-readable note explaining backend selection/fallback.
    [[nodiscard]] std::string_view IoBackendNote() const;

  private:
    WALStorage();

    std::unique_ptr<WAL> wal_;
    mutable std::mutex mutex_;

    // Protected by mutex_.
    WALIoBackend effective_io_backend_;
    std::string io_backend_note_;

    // Snapshot data (stored in memory, persisted separately)
    Snapshot snapshot_;
};

}  // namespace raftpp::raftor::wal
