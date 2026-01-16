#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace raftpp::wal {

// Index entry for fast lookup of entries in segments
struct IndexEntry {
    uint64_t segment_id;  // Which segment file contains this entry
    uint64_t offset;      // Byte offset within the segment
    uint32_t length;      // Total record length (header + payload + padding)
    uint64_t term;        // Entry term (cached for Term() queries)
};

// In-memory index for O(1) entry lookup by index
// The index is rebuilt on recovery by scanning all segments
class WALIndex {
  public:
    WALIndex() = default;

    // Insert an entry at the given index
    // Assumes entries are inserted in order (index == last_index + 1)
    void Insert(uint64_t index, uint64_t segment_id, uint64_t offset, uint32_t length, uint64_t term);

    // Lookup an entry by index
    [[nodiscard]] std::optional<IndexEntry> Lookup(uint64_t index) const;

    // Get the term for an entry at the given index
    [[nodiscard]] std::optional<uint64_t> Term(uint64_t index) const;

    // Truncate all entries starting from the given index (inclusive)
    // Used for recovery when detecting incomplete writes
    void TruncateFrom(uint64_t index);

    // Truncate all entries before the given index (exclusive)
    // Used for log compaction
    void TruncateBefore(uint64_t index);

    // Clear all entries
    void Clear();

    // Getters
    [[nodiscard]] uint64_t first_index() const { return first_index_; }
    [[nodiscard]] uint64_t last_index() const;
    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] size_t size() const { return entries_.size(); }

    // Set the first index (used when rebuilding from metadata)
    void SetFirstIndex(uint64_t first_index);

  private:
    uint64_t first_index_ = 1;  // First index in the index (default to 1)
    std::vector<IndexEntry> entries_;
};

}  // namespace raftpp::wal
