#pragma once

#include <sys/types.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <system_error>

namespace raftpp::raftor::wal {

class WALEnv {
  public:
    virtual ~WALEnv() = default;

    virtual std::error_code CreateDirectories(const std::filesystem::path& path) = 0;
    virtual int Open(const std::filesystem::path& path, int flags, mode_t mode) = 0;
    virtual ssize_t Write(int fd, const void* data, size_t size) = 0;
    virtual ssize_t Read(int fd, void* data, size_t size) = 0;
    virtual off_t Seek(int fd, off_t offset, int whence) = 0;
    virtual int Sync(int fd) = 0;
    virtual int Close(int fd) = 0;
    virtual int Rename(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
    virtual int Unlink(const std::filesystem::path& path) = 0;
};

[[nodiscard]] std::shared_ptr<WALEnv> DefaultWALEnv();

}  // namespace raftpp::raftor::wal
