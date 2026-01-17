#include "raftor/wal/segment.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <regex>

#include <spdlog/spdlog.h>

namespace raftpp::wal {

Segment::~Segment() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Segment::Segment(Segment&& other) noexcept
    : fd_(other.fd_),
      segment_id_(other.segment_id_),
      first_index_(other.first_index_),
      write_offset_(other.write_offset_),
      path_(std::move(other.path_)) {
    other.fd_ = -1;
}

Segment& Segment::operator=(Segment&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        segment_id_ = other.segment_id_;
        first_index_ = other.first_index_;
        write_offset_ = other.write_offset_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

Result<std::unique_ptr<Segment>> Segment::Create(
    const std::filesystem::path& path, uint64_t segment_id, uint64_t first_index, bool preallocate,
    uint64_t preallocate_size
) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return RaftError(
            StorageErrorOther{
                fmt::format("failed to create segment {}: {}", path.string(), strerror(errno))
            }
        );
    }

    // Preallocate space if requested
    if (preallocate && preallocate_size > 0) {
#ifdef __linux__
        if (::posix_fallocate(fd, 0, preallocate_size) != 0) {
            SPDLOG_WARN("posix_fallocate failed: {}", strerror(errno));
        }
#endif
    }

    // Write segment header
    SegmentHeader header;
    header.segment_id = segment_id;
    header.first_index = first_index;

    std::array<uint8_t, sizeof(SegmentHeader)> header_buf{};
    header.Serialize(std::span<uint8_t, 32>(header_buf));

    ssize_t written = ::write(fd, header_buf.data(), header_buf.size());
    if (written != static_cast<ssize_t>(header_buf.size())) {
        ::close(fd);
        ::unlink(path.c_str());
        return RaftError(
            StorageErrorOther{fmt::format("failed to write segment header: {}", strerror(errno))}
        );
    }

    auto segment = std::unique_ptr<Segment>(new Segment());
    segment->fd_ = fd;
    segment->segment_id_ = segment_id;
    segment->first_index_ = first_index;
    segment->write_offset_ = sizeof(SegmentHeader);
    segment->path_ = path;

    SPDLOG_DEBUG("created segment {} with first_index={}", path.string(), first_index);

    return segment;
}

Result<std::unique_ptr<Segment>> Segment::Open(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        return RaftError(
            StorageErrorOther{
                fmt::format("failed to open segment {}: {}", path.string(), strerror(errno))
            }
        );
    }

    // Read and verify header
    std::array<uint8_t, sizeof(SegmentHeader)> header_buf{};
    ssize_t n = ::pread(fd, header_buf.data(), header_buf.size(), 0);
    if (n != static_cast<ssize_t>(header_buf.size())) {
        ::close(fd);
        return RaftError(
            StorageErrorOther{fmt::format("failed to read segment header: {}", strerror(errno))}
        );
    }

    auto header = SegmentHeader::Deserialize(std::span<const uint8_t, 32>(header_buf));
    if (!header.IsValid()) {
        ::close(fd);
        return RaftError(StorageErrorCode::InvalidSegmentHeader);
    }

    // Get file size to determine write offset
    struct stat st{};
    if (::fstat(fd, &st) < 0) {
        ::close(fd);
        return RaftError(
            StorageErrorOther{fmt::format("failed to stat segment: {}", strerror(errno))}
        );
    }

    auto segment = std::unique_ptr<Segment>(new Segment());
    segment->fd_ = fd;
    segment->segment_id_ = header.segment_id;
    segment->first_index_ = header.first_index;
    segment->write_offset_ = static_cast<uint64_t>(st.st_size);
    segment->path_ = path;

    SPDLOG_DEBUG(
        "opened segment {} with segment_id={}, first_index={}, size={}", path.string(),
        header.segment_id, header.first_index, st.st_size
    );

    return segment;
}

Result<void> Segment::Append(std::span<const uint8_t> data) {
    if (fd_ < 0) {
        return RaftError(StorageErrorCode::SegmentNotOpen);
    }

    ssize_t written = ::pwrite(fd_, data.data(), data.size(), static_cast<off_t>(write_offset_));
    if (written != static_cast<ssize_t>(data.size())) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to write to segment: {}", strerror(errno))}
        );
    }

    write_offset_ += data.size();
    return {};
}

Result<std::vector<uint8_t>> Segment::Read(uint64_t offset, uint32_t length) const {
    if (fd_ < 0) {
        return RaftError(StorageErrorCode::SegmentNotOpen);
    }

    std::vector<uint8_t> data(length);
    ssize_t n = ::pread(fd_, data.data(), length, static_cast<off_t>(offset));
    if (n < 0) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to read from segment: {}", strerror(errno))}
        );
    }
    if (static_cast<uint32_t>(n) != length) {
        return RaftError(
            StorageErrorOther{
                fmt::format("short read from segment: expected {}, got {}", length, n)
            }
        );
    }

    return data;
}

Result<void> Segment::Sync() {
    if (fd_ < 0) {
        return RaftError(StorageErrorCode::SegmentNotOpen);
    }

    int rc = -1;

#if defined(__APPLE__) && defined(__MACH__)
    rc = ::fsync(fd_);
#else
    rc = ::fdatasync(fd_);
#endif

    if (rc < 0) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to sync segment: {}", strerror(errno))}
        );
    }
    return {};
}

Result<void> Segment::Truncate(uint64_t offset) {
    if (fd_ < 0) {
        return RaftError(StorageErrorCode::SegmentNotOpen);
    }

    if (::ftruncate(fd_, static_cast<off_t>(offset)) < 0) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to truncate segment: {}", strerror(errno))}
        );
    }

    write_offset_ = offset;
    SPDLOG_DEBUG("truncated segment {} to offset {}", path_.string(), offset);

    return {};
}

Result<void> Segment::Close() {
    if (fd_ >= 0) {
        if (::close(fd_) < 0) {
            return RaftError(
                StorageErrorOther{fmt::format("failed to close segment: {}", strerror(errno))}
            );
        }
        fd_ = -1;
    }
    return {};
}

std::optional<uint64_t> Segment::ParseSegmentId(const std::filesystem::path& path) {
    std::string filename = path.filename().string();
    static const std::regex pattern(R"(segment-(\d{6})\.wal)");
    std::smatch match;
    if (std::regex_match(filename, match, pattern)) {
        return std::stoull(match[1].str());
    }
    return std::nullopt;
}

std::string Segment::MakeSegmentFilename(uint64_t segment_id) {
    return fmt::format("segment-{:06d}.wal", segment_id);
}

}  // namespace raftpp::wal
