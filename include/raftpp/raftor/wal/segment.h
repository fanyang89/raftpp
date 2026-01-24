#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "raftpp/core/error.h"
#include "raftpp/raftor/wal/record.h"

namespace raftpp::raftor::wal {

// Represents a single WAL segment file
class Segment {
  public:
    ~Segment();

    // Disable copy
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    // Enable move
    Segment(Segment&& other) noexcept;
    Segment& operator=(Segment&& other) noexcept;

    // Create a new segment file
    [[nodiscard]] static Result<std::unique_ptr<Segment>> Create(
        const std::filesystem::path& path, uint64_t segment_id, uint64_t first_index,
        bool preallocate = true, uint64_t preallocate_size = 64 * 1024 * 1024
    );

    // Open an existing segment file for reading and appending
    [[nodiscard]] static Result<std::unique_ptr<Segment>> Open(const std::filesystem::path& path);

    // Append data to the segment
    [[nodiscard]] Result<void> Append(std::span<const uint8_t> data);

    // Read data from a specific offset
    [[nodiscard]] Result<std::vector<uint8_t>> Read(uint64_t offset, uint32_t length) const;

    // Sync data to disk
    [[nodiscard]] Result<void> Sync();

    // Truncate the segment at the given offset (for recovery)
    [[nodiscard]] Result<void> Truncate(uint64_t offset);

    // Close the segment file
    [[nodiscard]] Result<void> Close();

    // Getters
    [[nodiscard]] uint64_t segment_id() const { return segment_id_; }

    [[nodiscard]] uint64_t first_index() const { return first_index_; }

    [[nodiscard]] uint64_t write_offset() const { return write_offset_; }

    [[nodiscard]] uint64_t file_size() const { return file_size_; }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    // Check if segment has reached size threshold
    [[nodiscard]] bool IsFull(uint64_t threshold) const { return write_offset_ >= threshold; }

    // Parse segment filename to extract segment_id
    [[nodiscard]] static std::optional<uint64_t> ParseSegmentId(const std::filesystem::path& path);

    // Generate segment filename from segment_id
    [[nodiscard]] static std::string MakeSegmentFilename(uint64_t segment_id);

  private:
    Segment() = default;

    int fd_ = -1;
    uint64_t segment_id_ = 0;
    uint64_t first_index_ = 0;
    uint64_t write_offset_ = 0;
    uint64_t file_size_ = 0;
    std::filesystem::path path_;
};

}  // namespace raftpp::raftor::wal
