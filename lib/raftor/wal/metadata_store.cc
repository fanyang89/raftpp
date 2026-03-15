#include "raftpp/raftor/wal/metadata_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include "raftpp/core/capnp_util.h"
#include "raftpp/logging.h"
#include "raftpp/raftor/wal/crc32c.h"
#include "raftpp/raftor/wal/record.h"

namespace raftpp::raftor::wal {

MetadataStore::MetadataStore(const std::filesystem::path& dir)
    : path_(dir / "metadata"), tmp_path_(dir / "metadata.tmp") {}

Result<void> MetadataStore::Initialize() {
    // Create the directory if it doesn't exist
    std::error_code ec;
    if (!std::filesystem::exists(path_.parent_path())) {
        std::filesystem::create_directories(path_.parent_path(), ec);
        if (ec) {
            return RaftError(
                StorageErrorOther{fmt::format("failed to create directory: {}", ec.message())}
            );
        }
    }

    // If metadata file doesn't exist, create with default values
    if (!Exists()) {
        WALMetadata default_meta;
        auto result = Save(default_meta);
        if (!result) {
            return result;
        }
        RAFTPP_LOG_DEBUG("created default metadata file at {}", path_.string());
    }

    return {};
}

Result<WALMetadata> MetadataStore::Load() {
    if (!Exists()) {
        // Return default metadata
        return WALMetadata{};
    }

    // Read the file
    int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to open metadata: {}", strerror(errno))}
        );
    }

    // Get file size
    off_t size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) {
        ::close(fd);
        return RaftError(
            StorageErrorOther{fmt::format("failed to get metadata size: {}", strerror(errno))}
        );
    }
    ::lseek(fd, 0, SEEK_SET);

    // Read contents
    std::vector<uint8_t> data(size);
    ssize_t n = ::read(fd, data.data(), data.size());
    ::close(fd);

    if (n != size) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to read metadata: {}", strerror(errno))}
        );
    }

    return Deserialize(data);
}

Result<void> MetadataStore::Save(const WALMetadata& meta) {
    auto data = Serialize(meta);
    return AtomicWrite(data);
}

bool MetadataStore::Exists() const {
    return std::filesystem::exists(path_);
}

uint64_t MetadataStore::SizeBytes() const {
    std::error_code ec;
    uint64_t total = 0;
    if (std::filesystem::exists(path_, ec)) {
        auto size = std::filesystem::file_size(path_, ec);
        if (!ec) {
            total += size;
        }
    }
    ec.clear();
    if (std::filesystem::exists(tmp_path_, ec)) {
        auto size = std::filesystem::file_size(tmp_path_, ec);
        if (!ec) {
            total += size;
        }
    }
    return total;
}

Result<void> MetadataStore::AtomicWrite(const std::vector<uint8_t>& data) {
    // Write to temporary file
    int fd = ::open(tmp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return RaftError(
            StorageErrorOther{fmt::format("failed to create temp metadata: {}", strerror(errno))}
        );
    }

    ssize_t written = ::write(fd, data.data(), data.size());
    if (written != static_cast<ssize_t>(data.size())) {
        ::close(fd);
        ::unlink(tmp_path_.c_str());
        return RaftError(
            StorageErrorOther{fmt::format("failed to write temp metadata: {}", strerror(errno))}
        );
    }

    // Sync to disk
    if (::fsync(fd) < 0) {
        ::close(fd);
        ::unlink(tmp_path_.c_str());
        return RaftError(
            StorageErrorOther{fmt::format("failed to sync temp metadata: {}", strerror(errno))}
        );
    }

    ::close(fd);

    // Atomic rename
    if (::rename(tmp_path_.c_str(), path_.c_str()) < 0) {
        ::unlink(tmp_path_.c_str());
        return RaftError(
            StorageErrorOther{fmt::format("failed to rename metadata: {}", strerror(errno))}
        );
    }

    // Sync the directory to ensure the rename is durable
    int dir_fd = ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }

    RAFTPP_LOG_DEBUG("saved metadata to {}", path_.string());

    return {};
}

std::vector<uint8_t> MetadataStore::Serialize(const WALMetadata& meta) const {
    // Serialize Cap'n Proto messages
    std::vector<uint8_t> hard_state_bytes;
    if (meta.hard_state) {
        hard_state_bytes = capnp_util::toBytes(meta.hard_state);
    }
    std::vector<uint8_t> conf_state_bytes;
    if (meta.conf_state) {
        conf_state_bytes = capnp_util::toBytes(meta.conf_state);
    }

    // Calculate total size:
    // MetadataHeader (16) + MetadataContent (24) + hard_state_len (4) + hard_state + conf_state_len (4) + conf_state
    size_t total_size = sizeof(MetadataHeader) + sizeof(MetadataContent) + 4 +
        hard_state_bytes.size() + 4 + conf_state_bytes.size();

    std::vector<uint8_t> data(total_size, 0);
    size_t offset = 0;

    // Skip header for now (we'll fill in CRC later)
    MetadataHeader header;
    offset += sizeof(MetadataHeader);

    // Write MetadataContent
    MetadataContent content;
    content.first_index = meta.first_index;
    content.snapshot_index = meta.snapshot_index;
    content.snapshot_term = meta.snapshot_term;
    std::memcpy(data.data() + offset, &content, sizeof(MetadataContent));
    offset += sizeof(MetadataContent);

    // Write hard_state
    uint32_t hs_len = static_cast<uint32_t>(hard_state_bytes.size());
    std::memcpy(data.data() + offset, &hs_len, sizeof(hs_len));
    offset += sizeof(hs_len);
    if (hs_len > 0) {
        std::memcpy(data.data() + offset, hard_state_bytes.data(), hs_len);
        offset += hs_len;
    }

    // Write conf_state
    uint32_t cs_len = static_cast<uint32_t>(conf_state_bytes.size());
    std::memcpy(data.data() + offset, &cs_len, sizeof(cs_len));
    offset += sizeof(cs_len);
    if (cs_len > 0) {
        std::memcpy(data.data() + offset, conf_state_bytes.data(), cs_len);
        offset += cs_len;
    }

    // Compute CRC over everything after the CRC field
    size_t crc_offset = offsetof(MetadataHeader, crc) + sizeof(header.crc);
    CRC32C crc;
    crc.Update(data.data() + crc_offset, data.size() - crc_offset);
    header.crc = crc.Finalize();

    // Write header
    std::memcpy(data.data(), &header, sizeof(MetadataHeader));

    return data;
}

Result<WALMetadata> MetadataStore::Deserialize(const std::vector<uint8_t>& data) const {
    if (data.size() < sizeof(MetadataHeader) + sizeof(MetadataContent)) {
        return RaftError(StorageErrorCode::MetadataFileTooSmall);
    }

    const uint8_t* ptr = data.data();

    // Read and verify header
    MetadataHeader header;
    std::memcpy(&header, ptr, sizeof(MetadataHeader));
    ptr += sizeof(MetadataHeader);

    if (!header.IsValid()) {
        return RaftError(StorageErrorCode::InvalidMetadataHeader);
    }

    // Verify CRC
    size_t crc_offset = offsetof(MetadataHeader, crc) + sizeof(header.crc);
    CRC32C crc;
    crc.Update(data.data() + crc_offset, data.size() - crc_offset);
    if (crc.Finalize() != header.crc) {
        return RaftError(StorageErrorCode::MetadataCrcMismatch);
    }

    // Read MetadataContent
    MetadataContent content;
    std::memcpy(&content, ptr, sizeof(MetadataContent));
    ptr += sizeof(MetadataContent);

    WALMetadata meta;
    meta.first_index = content.first_index;
    meta.snapshot_index = content.snapshot_index;
    meta.snapshot_term = content.snapshot_term;

    // Read hard_state
    uint32_t hs_len;
    std::memcpy(&hs_len, ptr, sizeof(hs_len));
    ptr += sizeof(hs_len);

    try {
        if (hs_len > 0) {
            // Allocate aligned buffer and copy data
            kj::Array<::capnp::word> aligned_words = kj::heapArray<::capnp::word>((hs_len + 7) / 8);
            std::memcpy(aligned_words.begin(), ptr, hs_len);

            size_t word_count = hs_len / sizeof(::capnp::word);
            meta.hard_state = capnp_util::fromWords<msg::HardState>(
                kj::ArrayPtr<const ::capnp::word>(aligned_words.begin(), word_count)
            );
        }
    } catch (...) {
        return RaftError(StorageErrorCode::HardStateParseError);
    }
    ptr += hs_len;

    // Read conf_state
    uint32_t cs_len;
    std::memcpy(&cs_len, ptr, sizeof(cs_len));
    ptr += sizeof(cs_len);

    try {
        if (cs_len > 0) {
            // Allocate aligned buffer and copy data
            kj::Array<::capnp::word> aligned_words = kj::heapArray<::capnp::word>((cs_len + 7) / 8);
            std::memcpy(aligned_words.begin(), ptr, cs_len);

            size_t word_count = cs_len / sizeof(::capnp::word);
            meta.conf_state = capnp_util::fromWords<msg::ConfState>(
                kj::ArrayPtr<const ::capnp::word>(aligned_words.begin(), word_count)
            );
        }
    } catch (...) {
        return RaftError(StorageErrorCode::ConfStateParseError);
    }

    if (!meta.hard_state) {
        meta.hard_state = capnp_util::make<msg::HardState>();
    }
    if (!meta.conf_state) {
        meta.conf_state = capnp_util::make<msg::ConfState>();
    }

    auto hs_reader = capnp_util::reader<msg::HardState>(meta.hard_state);
    RAFTPP_LOG_DEBUG(
        "loaded metadata: first_index={}, snapshot_index={}, term={}, vote={}", meta.first_index,
        meta.snapshot_index, hs_reader.getTerm(), hs_reader.getVote()
    );

    return meta;
}

}  // namespace raftpp::raftor::wal
