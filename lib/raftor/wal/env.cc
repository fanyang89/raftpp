#include "raftpp/raftor/wal/env.h"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <system_error>

namespace raftpp::raftor::wal {

namespace {

class PosixWALEnv final : public WALEnv {
  public:
    std::error_code CreateDirectories(const std::filesystem::path& path) override {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return ec;
    }

    int Open(const std::filesystem::path& path, int flags, mode_t mode) override {
        return ::open(path.c_str(), flags, mode);
    }

    ssize_t Write(int fd, const void* data, size_t size) override {
        return ::write(fd, data, size);
    }

    ssize_t Read(int fd, void* data, size_t size) override { return ::read(fd, data, size); }

    off_t Seek(int fd, off_t offset, int whence) override { return ::lseek(fd, offset, whence); }

    int Sync(int fd) override { return ::fsync(fd); }

    int Close(int fd) override { return ::close(fd); }

    int Rename(const std::filesystem::path& from, const std::filesystem::path& to) override {
        return ::rename(from.c_str(), to.c_str());
    }

    int Unlink(const std::filesystem::path& path) override { return ::unlink(path.c_str()); }
};

}  // namespace

std::shared_ptr<WALEnv> DefaultWALEnv() {
    static auto env = std::make_shared<PosixWALEnv>();
    return env;
}

}  // namespace raftpp::raftor::wal
