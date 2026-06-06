#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace raftpp::raftor::wal {

class WALEnv;

enum class WALIoBackend {
    Auto,
    Posix,
    IoUring,
};

struct WALConfig {
    // WAL directory path
    std::filesystem::path dir;

    // I/O backend selection (default: Auto)
    WALIoBackend io_backend = WALIoBackend::Auto;

    // io_uring submission queue depth (default: 256)
    // Only used when io_backend == WALIoBackend::IoUring.
    uint32_t uring_queue_depth = 256;

    // Maximum segment file size (default: 64MB)
    uint64_t segment_size = 64 * 1024 * 1024;

    // Write buffer size for batching (default: 4MB)
    size_t write_buffer_size = 4 * 1024 * 1024;

    // Whether to sync after each write batch (default: true)
    bool sync_on_write = true;

    // Whether to preallocate segment files (default: true)
    bool preallocate = true;

    // Optional environment for snapshot file I/O. Defaults to POSIX syscalls.
    std::shared_ptr<WALEnv> env;
};

}  // namespace raftpp::raftor::wal
