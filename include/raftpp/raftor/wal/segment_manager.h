#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include "raftpp/core/error.h"
#include "raftpp/raftor/wal/segment.h"
#include "raftpp/raftor/wal/wal_config.h"

namespace raftpp::raftor::wal {

// Information about a segment for listing
struct SegmentInfo {
    uint64_t segment_id;
    uint64_t first_index;
    std::filesystem::path path;
};

// Manages the lifecycle of WAL segment files
class SegmentManager {
  public:
    SegmentManager(
        const std::filesystem::path& dir, const WALConfig& config,
        std::shared_ptr<SegmentIoFactory> io_factory
    );

    // Initialize by scanning existing segments
    [[nodiscard]] Result<void> Initialize();

    // Get the current (active) segment for writing
    // Creates a new segment if none exists
    [[nodiscard]] Result<Segment*> GetCurrentSegment(uint64_t first_index_hint);

    // Roll to a new segment
    [[nodiscard]] Result<Segment*> RollToNewSegment(uint64_t first_index);

    // Get the segment containing a specific index
    // Returns nullptr if not found
    [[nodiscard]] Segment* GetSegmentForIndex(uint64_t index);

    // Remove segments with segment_id less than the given value
    [[nodiscard]] Result<void> RemoveSegmentsBefore(uint64_t segment_id);

    // Remove a specific segment
    [[nodiscard]] Result<void> RemoveSegment(uint64_t segment_id);

    // Remove all segments
    [[nodiscard]] Result<void> RemoveAllSegments();

    // List all segments sorted by segment_id
    [[nodiscard]] std::vector<SegmentInfo> ListSegments() const;

    // Sum of segment file sizes (no filesystem scan)
    [[nodiscard]] uint64_t TotalSizeBytes() const;

    // Sync all segments
    [[nodiscard]] Result<void> SyncAll();

    // Close all segments
    [[nodiscard]] Result<void> CloseAll();

    // Get the next segment ID
    [[nodiscard]] uint64_t NextSegmentId() const;

    // Check if there are any segments
    [[nodiscard]] bool HasSegments() const { return !segments_.empty(); }

    // Access to segments map (for iteration during recovery)
    [[nodiscard]] const std::map<uint64_t, std::unique_ptr<Segment>>& segments() const {
        return segments_;
    }

  private:
    std::filesystem::path dir_;
    WALConfig config_;
    std::shared_ptr<SegmentIoFactory> io_factory_;
    std::map<uint64_t, std::unique_ptr<Segment>> segments_;  // segment_id -> Segment
    uint64_t current_segment_id_ = 0;
};

}  // namespace raftpp::raftor::wal
