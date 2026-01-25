#include "raftpp/raftor/wal/segment.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <regex>

#include <spdlog/spdlog.h>

#include "raftpp/raftor/wal/record.h"

#if !defined(RAFTPP_HAS_LIBURING)
#define RAFTPP_HAS_LIBURING 0
#endif

#if defined(__linux__) && RAFTPP_HAS_LIBURING
#include <liburing.h>

#include <mutex>
#endif

namespace raftpp::raftor::wal {

namespace {

class PosixSegmentIo final : public SegmentIo {
  public:
    Result<void> PWrite(int fd, uint64_t offset, std::span<const uint8_t> data) override {
        const ssize_t written = ::pwrite(fd, data.data(), data.size(), static_cast<off_t>(offset));
        if (written < 0) {
            return RaftError(StorageErrorOther{fmt::format("pwrite failed: {}", strerror(errno))});
        }
        if (static_cast<size_t>(written) != data.size()) {
            return RaftError(
                StorageErrorOther{
                    fmt::format("short write: expected {}, got {}", data.size(), written)
                }
            );
        }
        return {};
    }

    Result<void> PRead(int fd, uint64_t offset, std::span<uint8_t> data) override {
        const ssize_t n = ::pread(fd, data.data(), data.size(), static_cast<off_t>(offset));
        if (n < 0) {
            return RaftError(StorageErrorOther{fmt::format("pread failed: {}", strerror(errno))});
        }
        if (static_cast<size_t>(n) != data.size()) {
            return RaftError(
                StorageErrorOther{fmt::format("short read: expected {}, got {}", data.size(), n)}
            );
        }
        return {};
    }

    Result<void> Sync(int fd) override {
        int rc = -1;

#if defined(__APPLE__) && defined(__MACH__)
        rc = ::fsync(fd);
#else
        rc = ::fdatasync(fd);
#endif

        if (rc < 0) {
            return RaftError(StorageErrorOther{fmt::format("sync failed: {}", strerror(errno))});
        }
        return {};
    }
};

class PosixSegmentIoFactory final : public SegmentIoFactory {
  public:
    std::unique_ptr<SegmentIo> Create() const override {
        return std::make_unique<PosixSegmentIo>();
    }
};

std::string BuildIoUringFallbackNote(const RaftError& err) {
    if (err.Is(StorageErrorCode::IoUringNotLinux)) {
        return "io_uring not supported on this platform; falling back to posix";
    }
    if (err.Is(StorageErrorCode::IoUringNotBuilt)) {
        return "io_uring support not built; falling back to posix";
    }
    if (err.Is(StorageErrorCode::IoUringInitFailed)) {
        return "io_uring init failed; falling back to posix";
    }
    if (err.Is(StorageErrorCode::IoUringProbeMissingOp)) {
        return "io_uring missing required operations; falling back to posix";
    }
    return fmt::format("io_uring unavailable ({}); falling back to posix", err.ToString());
}

#if defined(__linux__) && RAFTPP_HAS_LIBURING

class IoUringEngine {
  public:
    ~IoUringEngine() {
        if (initialized_) {
            io_uring_queue_exit(&ring_);
        }
    }

    static Result<std::shared_ptr<IoUringEngine>> Create(uint32_t queue_depth) {
        if (queue_depth == 0) {
            return RaftError(StorageErrorCode::IoUringInitFailed);
        }

        struct EnableMakeShared final : public IoUringEngine {};

        auto engine = std::make_shared<EnableMakeShared>();
        const int rc = io_uring_queue_init(queue_depth, &engine->ring_, 0);
        if (rc < 0) {
            return RaftError(StorageErrorCode::IoUringInitFailed);
        }
        engine->initialized_ = true;

        io_uring_probe* probe = io_uring_get_probe_ring(&engine->ring_);
        if (!probe) {
            return RaftError(StorageErrorCode::IoUringProbeMissingOp);
        }

        const bool has_read = io_uring_opcode_supported(probe, IORING_OP_READ);
        const bool has_write = io_uring_opcode_supported(probe, IORING_OP_WRITE);
        const bool has_fsync = io_uring_opcode_supported(probe, IORING_OP_FSYNC);
        io_uring_free_probe(probe);

        if (!has_read || !has_write || !has_fsync) {
            return RaftError(StorageErrorCode::IoUringProbeMissingOp);
        }

        return engine;
    }

    Result<void> PWrite(int fd, uint64_t offset, std::span<const uint8_t> data) {
        std::lock_guard lock(mutex_);

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            return RaftError(StorageErrorOther{"io_uring_get_sqe failed for write"});
        }
        io_uring_prep_write(sqe, fd, data.data(), data.size(), static_cast<off_t>(offset));
        return SubmitAndWaitLocked("write", data.size());
    }

    Result<void> PRead(int fd, uint64_t offset, std::span<uint8_t> data) {
        std::lock_guard lock(mutex_);

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            return RaftError(StorageErrorOther{"io_uring_get_sqe failed for read"});
        }
        io_uring_prep_read(sqe, fd, data.data(), data.size(), static_cast<off_t>(offset));
        return SubmitAndWaitLocked("read", data.size());
    }

    Result<void> Sync(int fd) {
        std::lock_guard lock(mutex_);

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            return RaftError(StorageErrorOther{"io_uring_get_sqe failed for fsync"});
        }
        io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
        return SubmitAndWaitLocked("fsync", std::nullopt);
    }

  protected:
    IoUringEngine() = default;

  private:
    Result<void> SubmitAndWaitLocked(const char* op, std::optional<size_t> expected) {
        int submit_rc = 0;
        do {
            submit_rc = io_uring_submit(&ring_);
        } while (submit_rc == -EINTR);
        if (submit_rc < 0) {
            return RaftError(
                StorageErrorOther{fmt::format("io_uring_submit failed: {}", strerror(-submit_rc))}
            );
        }

        io_uring_cqe* cqe = nullptr;
        int wait_rc = 0;
        do {
            wait_rc = io_uring_wait_cqe(&ring_, &cqe);
        } while (wait_rc == -EINTR);
        if (wait_rc < 0) {
            return RaftError(
                StorageErrorOther{fmt::format("io_uring_wait_cqe failed: {}", strerror(-wait_rc))}
            );
        }

        const int res = cqe->res;
        io_uring_cqe_seen(&ring_, cqe);

        if (res < 0) {
            return RaftError(
                StorageErrorOther{fmt::format("io_uring {} failed: {}", op, strerror(-res))}
            );
        }

        if (expected && static_cast<size_t>(res) != *expected) {
            return RaftError(
                StorageErrorOther{fmt::format("short {}: expected {}, got {}", op, *expected, res)}
            );
        }

        return {};
    }

    io_uring ring_{};
    bool initialized_ = false;
    std::mutex mutex_;
};

class IoUringSegmentIo final : public SegmentIo {
  public:
    explicit IoUringSegmentIo(std::shared_ptr<IoUringEngine> engine) : engine_(std::move(engine)) {}

    Result<void> PWrite(int fd, uint64_t offset, std::span<const uint8_t> data) override {
        return engine_->PWrite(fd, offset, data);
    }

    Result<void> PRead(int fd, uint64_t offset, std::span<uint8_t> data) override {
        return engine_->PRead(fd, offset, data);
    }

    Result<void> Sync(int fd) override { return engine_->Sync(fd); }

  private:
    std::shared_ptr<IoUringEngine> engine_;
};

class IoUringSegmentIoFactory final : public SegmentIoFactory {
  public:
    explicit IoUringSegmentIoFactory(std::shared_ptr<IoUringEngine> engine)
        : engine_(std::move(engine)) {}

    std::unique_ptr<SegmentIo> Create() const override {
        return std::make_unique<IoUringSegmentIo>(engine_);
    }

  private:
    std::shared_ptr<IoUringEngine> engine_;
};

#endif  // defined(__linux__) && RAFTPP_HAS_LIBURING

}  // namespace

Result<SegmentIoBackendSelection> SelectSegmentIoBackend(const WALConfig& config) {
    SegmentIoBackendSelection selection;

    auto use_posix = [&selection]() {
        selection.io_factory = std::make_shared<PosixSegmentIoFactory>();
        selection.effective_backend = WALIoBackend::Posix;
    };

    if (config.io_backend == WALIoBackend::Posix) {
        use_posix();
        return selection;
    }

#if defined(__linux__) && RAFTPP_HAS_LIBURING
    auto engine_result = IoUringEngine::Create(config.uring_queue_depth);
    if (config.io_backend == WALIoBackend::IoUring) {
        if (!engine_result) {
            return engine_result.error();
        }

        selection.io_factory = std::make_shared<IoUringSegmentIoFactory>(std::move(*engine_result));
        selection.effective_backend = WALIoBackend::IoUring;
        return selection;
    }

    if (engine_result) {
        selection.io_factory = std::make_shared<IoUringSegmentIoFactory>(std::move(*engine_result));
        selection.effective_backend = WALIoBackend::IoUring;
        return selection;
    }

    use_posix();
    selection.note = BuildIoUringFallbackNote(engine_result.error());
    return selection;
#else
    RaftError error = []() -> RaftError {
#if defined(__linux__)
        return RaftError(StorageErrorCode::IoUringNotBuilt);
#else
        return RaftError(StorageErrorCode::IoUringNotLinux);
#endif
    }();

    if (config.io_backend == WALIoBackend::IoUring) {
        return error;
    }

    use_posix();
    selection.note = BuildIoUringFallbackNote(error);
    return selection;
#endif
}

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
      file_size_(other.file_size_),
      path_(std::move(other.path_)),
      io_(std::move(other.io_)) {
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
        file_size_ = other.file_size_;
        path_ = std::move(other.path_);
        io_ = std::move(other.io_);
        other.fd_ = -1;
    }
    return *this;
}

Result<std::unique_ptr<Segment>> Segment::Create(
    const std::filesystem::path& path, uint64_t segment_id, uint64_t first_index, bool preallocate,
    uint64_t preallocate_size, std::unique_ptr<SegmentIo> io
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

    struct stat st{};
    if (::fstat(fd, &st) < 0) {
        ::close(fd);
        ::unlink(path.c_str());
        return RaftError(
            StorageErrorOther{fmt::format("failed to stat segment: {}", strerror(errno))}
        );
    }

    auto segment = std::unique_ptr<Segment>(new Segment());
    segment->io_ = io ? std::move(io) : std::make_unique<PosixSegmentIo>();
    segment->fd_ = fd;
    segment->segment_id_ = segment_id;
    segment->first_index_ = first_index;
    segment->write_offset_ = sizeof(SegmentHeader);
    segment->file_size_ = static_cast<uint64_t>(st.st_size);
    segment->path_ = path;

    SPDLOG_DEBUG("created segment {} with first_index={}", path.string(), first_index);

    return segment;
}

Result<std::unique_ptr<Segment>> Segment::Open(
    const std::filesystem::path& path, std::unique_ptr<SegmentIo> io
) {
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
    segment->io_ = io ? std::move(io) : std::make_unique<PosixSegmentIo>();
    segment->fd_ = fd;
    segment->segment_id_ = header.segment_id;
    segment->first_index_ = header.first_index;
    segment->write_offset_ = static_cast<uint64_t>(st.st_size);
    segment->file_size_ = static_cast<uint64_t>(st.st_size);
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

    if (const auto result = io_->PWrite(fd_, write_offset_, data); !result) {
        return result.error();
    }

    write_offset_ += data.size();
    if (write_offset_ > file_size_) {
        file_size_ = write_offset_;
    }
    return {};
}

Result<std::vector<uint8_t>> Segment::Read(uint64_t offset, uint32_t length) const {
    if (fd_ < 0) {
        return RaftError(StorageErrorCode::SegmentNotOpen);
    }

    std::vector<uint8_t> data(length);
    if (const auto result = io_->PRead(fd_, offset, std::span<uint8_t>(data)); !result) {
        return result.error();
    }
    return data;
}

Result<void> Segment::Sync() {
    if (fd_ < 0) {
        return RaftError(StorageErrorCode::SegmentNotOpen);
    }

    if (const auto result = io_->Sync(fd_); !result) {
        return result.error();
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
    file_size_ = offset;
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

}  // namespace raftpp::raftor::wal
