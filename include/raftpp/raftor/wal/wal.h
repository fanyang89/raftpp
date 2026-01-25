#pragma once

#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <vector>

#include "raftpp/core/raft.h"
#include "raftpp/core/types.h"
#include "raftpp/raftor/wal/metadata_store.h"
#include "raftpp/raftor/wal/segment_manager.h"
#include "raftpp/raftor/wal/wal_config.h"
#include "raftpp/raftor/wal/wal_index.h"

namespace raftpp::raftor::wal {

// Core Write-Ahead Log implementation
// Provides durable storage for Raft log entries and hard state
class WAL {
  public:
    ~WAL();

    // Disable copy
    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    // Factory method to open or create a WAL
    [[nodiscard]] static Result<std::unique_ptr<WAL>> Open(const WALConfig& config);

    // Append entries to the log
    // Entries must be continuous with the current last_index
    [[nodiscard]] Result<void> Append(std::span<const Entry> entries);

    // Save hard state
    [[nodiscard]] Result<void> SaveHardState(const HardState& hs);

    // Save conf state
    [[nodiscard]] Result<void> SaveConfState(const ConfState& cs);

    // Read entries in the range [low, high)
    // If max_size is specified, returns at most that many bytes (but at least one entry)
    [[nodiscard]] Result<std::vector<Entry>> ReadEntries(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size
    ) const;

    // Get the term for an entry at the given index
    [[nodiscard]] Result<uint64_t> Term(uint64_t index) const;

    // Get the first available index
    [[nodiscard]] uint64_t FirstIndex() const;

    // Get the last available index
    [[nodiscard]] uint64_t LastIndex() const;

    // Get the current hard state
    [[nodiscard]] const HardState& GetHardState() const;

    // Get the current conf state
    [[nodiscard]] const ConfState& GetConfState() const;

    // Get approximate WAL size in bytes
    [[nodiscard]] uint64_t LogSizeBytes() const;

    // Compact the log by removing entries before compact_index
    [[nodiscard]] Result<void> Compact(uint64_t compact_index);

    // Apply a snapshot
    [[nodiscard]] Result<void> ApplySnapshot(const Snapshot& snapshot);

    // Sync all pending writes to disk
    [[nodiscard]] Result<void> Sync();

    // Close the WAL
    [[nodiscard]] Result<void> Close();

  private:
    WAL() = default;

    // Initialize the WAL (called from Open)
    [[nodiscard]] Result<void> Initialize(const WALConfig& config);

    // Recover state from existing WAL files
    [[nodiscard]] Result<void> Recover();

    // Replay entries from a segment during recovery
    [[nodiscard]] Result<void> ReplaySegment(Segment* segment);

    // Create WALMetadata from current state (no locking, caller must hold lock)
    WALMetadata CreateMetadata() const {
        WALMetadata meta;
        meta.hard_state = CloneHardState(hard_state_);
        meta.conf_state = CloneConfState(conf_state_);
        meta.first_index = first_index_;
        meta.snapshot_index = snapshot_index_;
        meta.snapshot_term = snapshot_term_;
        return meta;
    }

    // Write a record to the current segment
    [[nodiscard]] Result<void> WriteRecord(RecordType type, std::span<const uint8_t> data);

    // Flush the write buffer to the current segment
    [[nodiscard]] Result<void> FlushWriteBuffer();

    // Roll to a new segment if needed
    [[nodiscard]] Result<void> MaybeRollSegment();

    [[nodiscard]] Result<Segment*> GetCurrentSegmentForAppend(uint64_t first_index_hint);
    [[nodiscard]] Result<void> MaybeRollSegmentForAppend(uint64_t first_index, Segment*& segment);

    // Internal helpers (no locking, must be called with lock held)
    [[nodiscard]] uint64_t LastIndexUnlocked() const;
    [[nodiscard]] uint64_t FirstIndexUnlocked() const;

    WALConfig config_;
    std::unique_ptr<SegmentManager> segment_manager_;
    std::unique_ptr<MetadataStore> metadata_store_;
    WALIndex index_;

    // Current state
    HardState hard_state_;
    ConfState conf_state_;
    uint64_t first_index_ = 1;
    uint64_t snapshot_index_ = 0;
    uint64_t snapshot_term_ = 0;

    // Write buffer for batching
    std::vector<uint8_t> write_buffer_;
    size_t write_buffer_used_ = 0;

    struct PendingEntry {
        uint64_t index;
        uint64_t term;
        uint32_t offset_in_buffer;
        uint32_t record_length;
    };

    std::vector<PendingEntry> pending_entries_;

    [[nodiscard]] Result<void> FlushWriteBufferIfNeeded();
    [[nodiscard]] bool ShouldFlushBuffer(Segment* segment) const;

    // Thread safety
    mutable std::shared_mutex mutex_;
};

}  // namespace raftpp::raftor::wal
