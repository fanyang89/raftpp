#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace raftpp::wal {

struct WALConfig {
    // WAL directory path
    std::filesystem::path dir;

    // Maximum segment file size (default: 64MB)
    uint64_t segment_size = 64 * 1024 * 1024;

    // Write buffer size for batching (default: 4MB)
    size_t write_buffer_size = 4 * 1024 * 1024;

    // Whether to sync after each write batch (default: true)
    bool sync_on_write = true;

    // Whether to preallocate segment files (default: true)
    bool preallocate = true;
};

}  // namespace raftpp::wal
