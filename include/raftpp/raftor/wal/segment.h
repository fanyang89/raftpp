#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "raftpp/core/error.h"
#include "raftpp/raftor/wal/wal_config.h"

namespace raftpp::raftor::wal {

class SegmentIo {
  public:
    virtual ~SegmentIo() = default;

    virtual Result<void> PWrite(int fd, uint64_t offset, std::span<const uint8_t> data) = 0;
    virtual Result<void> PRead(int fd, uint64_t offset, std::span<uint8_t> data) = 0;
    virtual Result<void> Sync(int fd) = 0;
};

class SegmentIoFactory {
  public:
    virtual ~SegmentIoFactory() = default;

    [[nodiscard]] virtual std::unique_ptr<SegmentIo> Create() const = 0;
};

struct SegmentIoBackendSelection {
    std::shared_ptr<SegmentIoFactory> io_factory;
    WALIoBackend effective_backend = WALIoBackend::Auto;
    std::string note;
};

// Select the effective I/O backend and create its factory.
[[nodiscard]] Result<SegmentIoBackendSelection> SelectSegmentIoBackend(const WALConfig& config);

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
        bool preallocate = true, uint64_t preallocate_size = 64 * 1024 * 1024,
        std::unique_ptr<SegmentIo> io = nullptr
    );

    // Open an existing segment file for reading and appending
    [[nodiscard]] static Result<std::unique_ptr<Segment>> Open(
        const std::filesystem::path& path, std::unique_ptr<SegmentIo> io = nullptr
    );

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

    std::unique_ptr<SegmentIo> io_;
};

}  // namespace raftpp::raftor::wal
