#include "raftpp/raftor/wal/wal.h"

#include <fcntl.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <doctest/doctest.h>
#include <kj/array.h>

#include "raftpp/raftor/wal/crc32c.h"
#include "raftpp/raftor/wal/env.h"
#include "raftpp/raftor/wal/metadata_store.h"
#include "raftpp/raftor/wal/record.h"
#include "raftpp/raftor/wal/segment.h"
#include "raftpp/raftor/wal/wal_index.h"
#include "raftpp/raftor/wal/wal_storage.h"

using namespace raftpp;
using namespace raftor::wal;

namespace {

// Helper to create a temporary directory for testing
class TempDir {
  public:
    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 999999);
        path_ = std::filesystem::temp_directory_path() /
            ("raftpp_wal_test_" + std::to_string(dis(gen)));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

// Helper to create an Entry
Entry MakeWalEntry(uint64_t index, uint64_t term, const std::string& data = "") {
    Entry entry = capnp_util::make<msg::Entry>();
    auto builder = capnp_util::builder<msg::Entry>(entry);
    builder.setIndex(index);
    builder.setTerm(term);
    builder.setData(kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size()));
    builder.setEntryType(EntryType::ENTRY_NORMAL);
    return entry;
}

Snapshot MakeWalSnapshot(
    uint64_t index, uint64_t term, std::vector<uint64_t> voters, const std::string& data
) {
    Snapshot snapshot = capnp_util::make<msg::Snapshot>();
    auto builder = capnp_util::builder<msg::Snapshot>(snapshot);
    builder.setData(kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size()));

    auto meta_builder = builder.initMetadata();
    meta_builder.setIndex(index);
    meta_builder.setTerm(term);
    auto conf_builder = meta_builder.initConfState();
    auto voters_builder = conf_builder.initVoters(voters.size());
    for (size_t i = 0; i < voters.size(); ++i) {
        voters_builder.set(i, voters[i]);
    }
    return snapshot;
}

void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(
        reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())
    );
    REQUIRE(out.good());
}

std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    );
}

void RefreshMetadataCrc(std::vector<uint8_t>& data) {
    REQUIRE(data.size() >= sizeof(MetadataHeader));
    MetadataHeader header;
    std::memcpy(&header, data.data(), sizeof(header));
    const size_t crc_offset = offsetof(MetadataHeader, crc) + sizeof(header.crc);
    CRC32C crc;
    crc.Update(data.data() + crc_offset, data.size() - crc_offset);
    header.crc = crc.Finalize();
    std::memcpy(data.data(), &header, sizeof(header));
}

class FailingSnapshotEnv : public WALEnv {
  public:
    std::error_code CreateDirectories(const std::filesystem::path& path) override {
        if (fail_create_directories) {
            return std::make_error_code(std::errc::permission_denied);
        }
        return delegate_->CreateDirectories(path);
    }

    int Open(const std::filesystem::path& path, int flags, mode_t mode) override {
        if (IsSnapshotPath(path) && fail_open_snapshot) {
            errno = EACCES;
            return -1;
        }
        if (IsSnapshotTmpPath(path) && fail_open_snapshot_tmp) {
            errno = EACCES;
            return -1;
        }
        if ((flags & O_DIRECTORY) != 0 && fail_open_directory) {
            errno = EACCES;
            return -1;
        }

        const int fd = delegate_->Open(path, flags, mode);
        if (fd >= 0) {
            fd_kind_[fd] = KindFor(path, flags);
        }
        return fd;
    }

    ssize_t Write(int fd, const void* data, size_t size) override {
        if (fd_kind_[fd] == FdKind::SnapshotTmp) {
            if (write_eintr_once) {
                write_eintr_once = false;
                errno = EINTR;
                return -1;
            }
            if (fail_write_once) {
                fail_write_once = false;
                errno = EIO;
                return -1;
            }
            if (zero_write_once) {
                zero_write_once = false;
                return 0;
            }
        }
        return delegate_->Write(fd, data, size);
    }

    ssize_t Read(int fd, void* data, size_t size) override {
        if (fd_kind_[fd] == FdKind::Snapshot) {
            if (read_eintr_once) {
                read_eintr_once = false;
                errno = EINTR;
                return -1;
            }
            if (fail_read_once) {
                fail_read_once = false;
                errno = EIO;
                return -1;
            }
            if (eof_read_once) {
                eof_read_once = false;
                return 0;
            }
        }
        return delegate_->Read(fd, data, size);
    }

    off_t Seek(int fd, off_t offset, int whence) override {
        if (fd_kind_[fd] == FdKind::Snapshot) {
            if (whence == SEEK_END && fail_seek_end) {
                errno = EIO;
                return -1;
            }
            if (whence == SEEK_SET && fail_seek_set) {
                errno = EIO;
                return -1;
            }
        }
        return delegate_->Seek(fd, offset, whence);
    }

    int Sync(int fd) override {
        if (fd_kind_[fd] == FdKind::SnapshotTmp && fail_sync_snapshot_tmp) {
            errno = EIO;
            return -1;
        }
        if (fd_kind_[fd] == FdKind::Directory && fail_sync_directory) {
            errno = EIO;
            return -1;
        }
        return delegate_->Sync(fd);
    }

    int Close(int fd) override {
        const auto it = fd_kind_.find(fd);
        const FdKind kind = it == fd_kind_.end() ? FdKind::Other : it->second;
        if (it != fd_kind_.end()) {
            fd_kind_.erase(it);
        }
        const int close_result = delegate_->Close(fd);
        if (kind == FdKind::SnapshotTmp && fail_close_snapshot_tmp) {
            errno = EIO;
            return -1;
        }
        return close_result;
    }

    int Rename(const std::filesystem::path& from, const std::filesystem::path& to) override {
        if (IsSnapshotTmpPath(from) && IsSnapshotPath(to) && fail_rename_snapshot) {
            errno = EIO;
            return -1;
        }
        return delegate_->Rename(from, to);
    }

    int Unlink(const std::filesystem::path& path) override { return delegate_->Unlink(path); }

    bool fail_create_directories = false;
    bool fail_open_snapshot = false;
    bool fail_open_snapshot_tmp = false;
    bool fail_open_directory = false;
    bool fail_seek_end = false;
    bool fail_seek_set = false;
    bool fail_write_once = false;
    bool write_eintr_once = false;
    bool zero_write_once = false;
    bool fail_read_once = false;
    bool read_eintr_once = false;
    bool eof_read_once = false;
    bool fail_sync_snapshot_tmp = false;
    bool fail_close_snapshot_tmp = false;
    bool fail_rename_snapshot = false;
    bool fail_sync_directory = false;

  private:
    enum class FdKind {
        Other,
        Snapshot,
        SnapshotTmp,
        Directory,
    };

    static bool IsSnapshotPath(const std::filesystem::path& path) {
        return path.filename() == "snapshot";
    }

    static bool IsSnapshotTmpPath(const std::filesystem::path& path) {
        return path.filename() == "snapshot.tmp";
    }

    static FdKind KindFor(const std::filesystem::path& path, int flags) {
        if ((flags & O_DIRECTORY) != 0) {
            return FdKind::Directory;
        }
        if (IsSnapshotPath(path)) {
            return FdKind::Snapshot;
        }
        if (IsSnapshotTmpPath(path)) {
            return FdKind::SnapshotTmp;
        }
        return FdKind::Other;
    }

    std::shared_ptr<WALEnv> delegate_ = DefaultWALEnv();
    std::unordered_map<int, FdKind> fd_kind_;
};

void PersistSnapshotForLoad(const std::filesystem::path& dir) {
    WALConfig config;
    config.dir = dir;
    config.sync_on_write = true;

    auto wal = WAL::Open(config);
    REQUIRE(wal.has_value());
    auto snap = MakeWalSnapshot(7, 3, {1}, "snapshot-payload");
    REQUIRE((*wal)->ApplySnapshot(snap));
}

Result<void> ApplySnapshotWithEnv(
    const std::filesystem::path& dir, const std::shared_ptr<FailingSnapshotEnv>& env
) {
    WALConfig config;
    config.dir = dir;
    config.sync_on_write = true;
    config.env = env;

    auto wal = WAL::Open(config);
    if (!wal) {
        return wal.error();
    }
    auto snap = MakeWalSnapshot(7, 3, {1}, "snapshot-payload");
    return (*wal)->ApplySnapshot(snap);
}

Result<Snapshot> LoadSnapshotWithEnv(
    const std::filesystem::path& dir, const std::shared_ptr<FailingSnapshotEnv>& env
) {
    WALConfig config;
    config.dir = dir;
    config.sync_on_write = true;
    config.env = env;

    auto wal = WAL::Open(config);
    if (!wal) {
        return wal.error();
    }
    return (*wal)->LoadSnapshot();
}

}  // namespace

TEST_SUITE("wal") {

    TEST_CASE("crc32c: basic computation") {
        const char* data = "hello world";
        uint32_t crc = CRC32C::Compute(data, strlen(data));
        CHECK(crc != 0);

        // Verify consistency
        uint32_t crc2 = CRC32C::Compute(data, strlen(data));
        CHECK(crc == crc2);
    }

    TEST_CASE("crc32c: incremental update") {
        const char* data = "hello world";
        uint32_t crc1 = CRC32C::Compute(data, strlen(data));

        CRC32C crc;
        crc.Update("hello ", 6);
        crc.Update("world", 5);
        uint32_t crc2 = crc.Finalize();

        CHECK(crc1 == crc2);
    }

    TEST_CASE("record: build and parse") {
        RecordBuilder builder;
        builder.SetType(RecordType::Entry);
        builder.SetPayload("test payload");
        auto record = builder.Build();

        CHECK(record.size() >= sizeof(RecordHeader));

        RecordParser parser(record);
        CHECK(parser.IsValid());
        CHECK(parser.Type() == RecordType::Entry);
        CHECK(parser.Payload().size() == 12);
        CHECK(std::string(parser.Payload().begin(), parser.Payload().end()) == "test payload");
    }

    TEST_CASE("record: corrupt data detection") {
        RecordBuilder builder;
        builder.SetType(RecordType::Entry);
        builder.SetPayload("test payload");
        auto record = builder.Build();

        // Corrupt the payload
        record[sizeof(RecordHeader) + 5] ^= 0xFF;

        RecordParser parser(record);
        CHECK(!parser.IsValid());
    }

    TEST_CASE("wal_index: insert and lookup") {
        WALIndex index;

        index.Insert(1, 1, 100, 50, 1);
        index.Insert(2, 1, 150, 50, 1);
        index.Insert(3, 1, 200, 50, 2);

        CHECK(index.first_index() == 1);
        CHECK(index.last_index() == 3);
        CHECK(index.size() == 3);

        auto entry = index.Lookup(2);
        REQUIRE(entry.has_value());
        CHECK(entry->segment_id == 1);
        CHECK(entry->offset == 150);
        CHECK(entry->term == 1);

        auto term = index.Term(3);
        REQUIRE(term.has_value());
        CHECK(*term == 2);
    }

    TEST_CASE("wal_index: truncate from") {
        WALIndex index;

        for (uint64_t i = 1; i <= 10; ++i) {
            index.Insert(i, 1, i * 100, 50, 1);
        }

        CHECK(index.last_index() == 10);

        index.TruncateFrom(6);
        CHECK(index.last_index() == 5);
        CHECK(!index.Lookup(6).has_value());
    }

    TEST_CASE("wal_index: truncate before") {
        WALIndex index;

        for (uint64_t i = 1; i <= 10; ++i) {
            index.Insert(i, 1, i * 100, 50, 1);
        }

        index.TruncateBefore(5);
        CHECK(index.first_index() == 5);
        CHECK(index.last_index() == 10);
        CHECK(!index.Lookup(4).has_value());
        CHECK(index.Lookup(5).has_value());
    }

    TEST_CASE("segment: create and write") {
        TempDir temp_dir;
        auto path = temp_dir.path() / "segment-000001.wal";

        auto segment = Segment::Create(path, 1, 1, false, 0);
        REQUIRE(segment.has_value());

        CHECK((*segment)->segment_id() == 1);
        CHECK((*segment)->first_index() == 1);
        CHECK((*segment)->write_offset() == sizeof(SegmentHeader));

        // Write some data
        std::vector<uint8_t> data = {1, 2, 3, 4, 5};
        auto result = (*segment)->Append(data);
        CHECK(result.has_value());
        CHECK((*segment)->write_offset() == sizeof(SegmentHeader) + 5);

        // Read it back
        auto read_result = (*segment)->Read(sizeof(SegmentHeader), 5);
        REQUIRE(read_result.has_value());
        CHECK(*read_result == data);
    }

    TEST_CASE("segment: open existing") {
        TempDir temp_dir;
        auto path = temp_dir.path() / "segment-000001.wal";

        // Create and write
        {
            auto segment = Segment::Create(path, 1, 1, false, 0);
            REQUIRE(segment.has_value());
            std::vector<uint8_t> data = {1, 2, 3, 4, 5};
            CHECK((*segment)->Append(data).has_value());
            CHECK((*segment)->Sync().has_value());
        }

        // Open and verify
        auto segment = Segment::Open(path);
        REQUIRE(segment.has_value());
        CHECK((*segment)->segment_id() == 1);
        CHECK((*segment)->first_index() == 1);

        auto read_result = (*segment)->Read(sizeof(SegmentHeader), 5);
        REQUIRE(read_result.has_value());
        CHECK((*read_result)[0] == 1);
    }

    TEST_CASE("segment: parse filename") {
        auto id = Segment::ParseSegmentId("segment-000001.wal");
        REQUIRE(id.has_value());
        CHECK(*id == 1);

        id = Segment::ParseSegmentId("segment-000123.wal");
        REQUIRE(id.has_value());
        CHECK(*id == 123);

        id = Segment::ParseSegmentId("invalid.wal");
        CHECK(!id.has_value());
    }

    TEST_CASE("wal: basic append and read") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.segment_size = 1024 * 1024;
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Append entries
        std::vector<Entry> entries;
        entries.push_back(MakeWalEntry(1, 1, "entry1"));
        entries.push_back(MakeWalEntry(2, 1, "entry2"));
        entries.push_back(MakeWalEntry(3, 2, "entry3"));

        auto append_result = (*wal)->Append(entries);
        CHECK(append_result.has_value());

        CHECK((*wal)->FirstIndex() == 1);
        CHECK((*wal)->LastIndex() == 3);

        // Read entries back
        auto read_result = (*wal)->ReadEntries(1, 4, std::nullopt);
        REQUIRE(read_result.has_value());
        CHECK(read_result->size() == 3);
        CHECK(capnp_util::reader<msg::Entry>((*read_result)[0]).getIndex() == 1);
        auto data = capnp_util::reader<msg::Entry>((*read_result)[0]).getData();
        CHECK(std::string(reinterpret_cast<const char*>(data.begin()), data.size()) == "entry1");
        CHECK(capnp_util::reader<msg::Entry>((*read_result)[2]).getIndex() == 3);
        CHECK(capnp_util::reader<msg::Entry>((*read_result)[2]).getTerm() == 2);
    }

    TEST_CASE("wal: term lookup") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        std::vector<Entry> entries;
        entries.push_back(MakeWalEntry(1, 1));
        entries.push_back(MakeWalEntry(2, 1));
        entries.push_back(MakeWalEntry(3, 2));
        CHECK((*wal)->Append(entries));

        auto term = (*wal)->Term(1);
        REQUIRE(term.has_value());
        CHECK(*term == 1);

        term = (*wal)->Term(3);
        REQUIRE(term.has_value());
        CHECK(*term == 2);

        // Compacted entry
        term = (*wal)->Term(0);
        CHECK(!term.has_value());
    }

    TEST_CASE("wal: hard state persistence") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        // Save hard state (with entries to satisfy commit invariant)
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            // First add entries so that commit can be valid
            std::vector<Entry> entries;
            for (uint64_t i = 1; i <= 10; ++i) {
                entries.push_back(MakeWalEntry(i, 5, "data"));
            }
            auto append_result = (*wal)->Append(entries);
            REQUIRE(append_result.has_value());

            HardState hs = capnp_util::make<msg::HardState>();
            auto hs_builder = capnp_util::builder<msg::HardState>(hs);
            hs_builder.setTerm(5);
            hs_builder.setVote(2);
            hs_builder.setCommit(10);

            auto result = (*wal)->SaveHardState(hs);
            CHECK(result.has_value());
        }

        // Reopen and verify
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            const auto& hs = (*wal)->GetHardState();
            auto hs_reader = capnp_util::reader<msg::HardState>(hs);
            CHECK(hs_reader.getTerm() == 5);
            CHECK(hs_reader.getVote() == 2);
            CHECK(hs_reader.getCommit() == 10);
        }
    }

    TEST_CASE("wal: recovery after restart") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        // Write entries
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            std::vector<Entry> entries;
            for (uint64_t i = 1; i <= 100; ++i) {
                entries.push_back(MakeWalEntry(i, 1, "data" + std::to_string(i)));
            }
            CHECK((*wal)->Append(entries));
        }

        // Reopen and verify
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            CHECK((*wal)->FirstIndex() == 1);
            CHECK((*wal)->LastIndex() == 100);

            auto read_result = (*wal)->ReadEntries(50, 60, std::nullopt);
            REQUIRE(read_result.has_value());
            CHECK(read_result->size() == 10);
            CHECK(capnp_util::reader<msg::Entry>((*read_result)[0]).getIndex() == 50);
        }
    }

    TEST_CASE("wal: compact") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Append entries
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 10; ++i) {
            entries.push_back(MakeWalEntry(i, 1));
        }
        CHECK((*wal)->Append(entries));

        // Compact
        auto result = (*wal)->Compact(5);
        CHECK(result.has_value());

        CHECK((*wal)->FirstIndex() == 5);
        CHECK((*wal)->LastIndex() == 10);

        // Old entries are compacted
        auto term = (*wal)->Term(4);
        CHECK(!term.has_value());
        CHECK(term.error() == StorageErrorCode::Compacted);

        // New entries still accessible
        term = (*wal)->Term(5);
        CHECK(term.has_value());
    }

    TEST_CASE("wal_storage: implements storage interface") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto storage = WALStorage::Open(config);
        REQUIRE(storage.has_value());

        // Test InitialState
        auto state = (*storage)->InitialState();
        REQUIRE(state.has_value());

        // Append entries
        std::vector<Entry> entries;
        entries.push_back(MakeWalEntry(1, 1, "entry1"));
        entries.push_back(MakeWalEntry(2, 1, "entry2"));

        auto append_result = (*storage)->Append(entries);
        CHECK(append_result.has_value());

        // Test Entries
        auto entries_result =
            (*storage)->Entries(1, 3, std::nullopt, GetEntriesContext::Empty(false));
        REQUIRE(entries_result.has_value());
        CHECK(entries_result->size() == 2);

        // Test Term
        auto term = (*storage)->Term(1);
        REQUIRE(term.has_value());
        CHECK(*term == 1);

        // Test FirstIndex and LastIndex
        auto first = (*storage)->FirstIndex();
        REQUIRE(first.has_value());
        CHECK(*first == 1);

        auto last = (*storage)->LastIndex();
        REQUIRE(last.has_value());
        CHECK(*last == 2);
    }

    TEST_CASE("wal_storage: io backend selection") {
        {
            TempDir temp_dir;

            WALConfig config;
            config.dir = temp_dir.path();
            config.sync_on_write = false;
            config.io_backend = WALIoBackend::Posix;

            auto storage = WALStorage::Open(config);
            REQUIRE(storage.has_value());
            CHECK((*storage)->EffectiveIoBackend() == WALIoBackend::Posix);
            CHECK((*storage)->IoBackendNote().empty());
        }

        {
            TempDir temp_dir;

            WALConfig config;
            config.dir = temp_dir.path();
            config.sync_on_write = false;
            config.io_backend = WALIoBackend::Auto;

            auto storage = WALStorage::Open(config);
            REQUIRE(storage.has_value());

            auto backend = (*storage)->EffectiveIoBackend();
            CHECK((backend == WALIoBackend::Posix || backend == WALIoBackend::IoUring));

            if (backend == WALIoBackend::Posix) {
                CHECK(!(*storage)->IoBackendNote().empty());
            } else {
                CHECK((*storage)->IoBackendNote().empty());
            }
        }

        {
            TempDir temp_dir;

            WALConfig config;
            config.dir = temp_dir.path();
            config.sync_on_write = false;
            config.io_backend = WALIoBackend::IoUring;

            auto storage = WALStorage::Open(config);
            if (storage) {
                CHECK((*storage)->EffectiveIoBackend() == WALIoBackend::IoUring);
                CHECK((*storage)->IoBackendNote().empty());
            } else {
                const auto& err = storage.error();
                bool matched = err.Is(StorageErrorCode::IoUringNotBuilt) ||
                    err.Is(StorageErrorCode::IoUringNotLinux) ||
                    err.Is(StorageErrorCode::IoUringInitFailed) ||
                    err.Is(StorageErrorCode::IoUringProbeMissingOp);
                CHECK(matched);
            }
        }
    }

    TEST_CASE("wal_storage: set hard state") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto storage = WALStorage::Open(config);
        REQUIRE(storage.has_value());

        HardState hs = capnp_util::make<msg::HardState>();
        auto hs_builder = capnp_util::builder<msg::HardState>(hs);
        hs_builder.setTerm(3);
        hs_builder.setVote(1);
        hs_builder.setCommit(5);

        (*storage)->SetHardState(std::move(hs));

        auto state = (*storage)->InitialState();
        REQUIRE(state.has_value());
        auto hs_reader = capnp_util::reader<msg::HardState>(state->hard_state);
        CHECK(hs_reader.getTerm() == 3);
        CHECK(hs_reader.getVote() == 1);
    }

    TEST_CASE("wal: size limit on entries") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Append entries with varying sizes
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 10; ++i) {
            entries.push_back(MakeWalEntry(i, 1, std::string(100, 'x')));
        }
        CHECK((*wal)->Append(entries));

        // Read with size limit - should return at least one entry
        auto read_result = (*wal)->ReadEntries(1, 11, 50);
        REQUIRE(read_result.has_value());
        CHECK(read_result->size() >= 1);
        CHECK(read_result->size() < 10);
    }

    TEST_CASE("wal: batch write basic") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.write_buffer_size = 1024;
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        std::vector<Entry> entries;
        for (int i = 0; i < 10; ++i) {
            entries.push_back(MakeWalEntry(i + 1, 1, "data"));
        }

        auto append_result = (*wal)->Append(entries);
        CHECK(append_result.has_value());

        auto read_result = (*wal)->ReadEntries(1, 11, std::nullopt);
        REQUIRE(read_result.has_value());
        CHECK(read_result->size() == 10);

        for (size_t i = 0; i < read_result->size(); ++i) {
            auto entry_reader = capnp_util::reader<msg::Entry>((*read_result)[i]);
            CHECK(entry_reader.getIndex() == i + 1);
            CHECK(entry_reader.getTerm() == 1);
        }
    }

    TEST_CASE("wal: flush triggers at buffer threshold") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.write_buffer_size = 200;
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        std::vector<Entry> entries;
        for (int i = 0; i < 10; ++i) {
            entries.push_back(MakeWalEntry(i + 1, 1, "test data payload"));
        }

        auto append_result = (*wal)->Append(entries);
        CHECK(append_result.has_value());

        auto read_result = (*wal)->ReadEntries(1, 11, std::nullopt);
        REQUIRE(read_result.has_value());
        CHECK(read_result->size() == 10);
    }

    TEST_CASE("wal: recovery after batch write") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            std::vector<Entry> entries;
            for (int i = 0; i < 100; ++i) {
                entries.push_back(MakeWalEntry(i + 1, 1, "data"));
            }

            auto append_result = (*wal)->Append(entries);
            CHECK(append_result.has_value());

            auto close_result = (*wal)->Close();
            CHECK(close_result.has_value());
        }

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        CHECK((*wal)->FirstIndex() == 1);
        CHECK((*wal)->LastIndex() == 100);

        auto read_result = (*wal)->ReadEntries(1, 101, std::nullopt);
        REQUIRE(read_result.has_value());
        CHECK(read_result->size() == 100);
    }

    TEST_CASE("wal: segment roll with batch write") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.segment_size = 1000;
        config.write_buffer_size = 500;
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        std::vector<Entry> entries;
        for (int i = 0; i < 50; ++i) {
            entries.push_back(MakeWalEntry(i + 1, 1, "data data data"));
        }

        auto append_result = (*wal)->Append(entries);
        CHECK(append_result.has_value());

        auto read_result = (*wal)->ReadEntries(1, 51, std::nullopt);
        REQUIRE(read_result.has_value());
        CHECK(read_result->size() == 50);
    }

    TEST_CASE("wal: sync with batch write") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.write_buffer_size = 4096;
        config.sync_on_write = true;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        std::vector<Entry> entries;
        for (int i = 0; i < 5; ++i) {
            entries.push_back(MakeWalEntry(i + 1, 1, "data"));
        }

        auto append_result = (*wal)->Append(entries);
        CHECK(append_result.has_value());

        auto sync_result = (*wal)->Sync();
        CHECK(sync_result.has_value());

        auto close_result = (*wal)->Close();
        CHECK(close_result.has_value());

        auto wal2 = WAL::Open(config);
        REQUIRE(wal2.has_value());
        CHECK((*wal2)->LastIndex() == 5);
    }

    TEST_CASE("wal: append entry larger than write buffer") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.write_buffer_size = 64;
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        std::string big_payload(4096, 'x');
        std::vector<Entry> entries;
        entries.push_back(MakeWalEntry(1, 1, big_payload));

        auto append_result = (*wal)->Append(entries);
        CHECK(append_result.has_value());
        CHECK((*wal)->LastIndex() == 1);

        auto read_result = (*wal)->ReadEntries(1, 2, std::nullopt);
        REQUIRE(read_result.has_value());
        REQUIRE(read_result->size() == 1);

        auto entry_reader = capnp_util::reader<msg::Entry>((*read_result)[0]);
        CHECK(entry_reader.getIndex() == 1);
        CHECK(entry_reader.getTerm() == 1);

        auto data = entry_reader.getData();
        CHECK(std::string(reinterpret_cast<const char*>(data.begin()), data.size()) == big_payload);
    }

    TEST_CASE("wal: save hard state larger than write buffer") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.write_buffer_size = 1;
        config.sync_on_write = true;

        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            HardState hs = capnp_util::make<msg::HardState>();
            auto hs_builder = capnp_util::builder<msg::HardState>(hs);
            hs_builder.setTerm(2);
            hs_builder.setVote(1);
            hs_builder.setCommit(0);

            auto save_result = (*wal)->SaveHardState(hs);
            CHECK(save_result.has_value());
            auto close_result = (*wal)->Close();
            CHECK(close_result.has_value());
        }

        auto wal2 = WAL::Open(config);
        REQUIRE(wal2.has_value());
        const auto& hs2 = (*wal2)->GetHardState();
        auto hs_reader = capnp_util::reader<msg::HardState>(hs2);
        CHECK(hs_reader.getTerm() == 2);
        CHECK(hs_reader.getVote() == 1);
        CHECK(hs_reader.getCommit() == 0);
    }

    // ============================================================================
    // Snapshot Tests
    // ============================================================================

    TEST_CASE("wal: apply snapshot basic") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Append some entries first
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 10; ++i) {
            entries.push_back(MakeWalEntry(i, 1, "data"));
        }
        CHECK((*wal)->Append(entries));

        CHECK((*wal)->FirstIndex() == 1);
        CHECK((*wal)->LastIndex() == 10);

        // Create and apply snapshot at index 5
        // Note: ApplySnapshot clears all entries and resets to snapshot index + 1
        Snapshot snap = capnp_util::make<msg::Snapshot>();
        auto snap_builder = capnp_util::builder<msg::Snapshot>(snap);
        auto meta_builder = snap_builder.initMetadata();
        meta_builder.setIndex(5);
        meta_builder.setTerm(1);

        auto result = (*wal)->ApplySnapshot(snap);
        CHECK(result.has_value());

        // After snapshot, first index should be snapshot index + 1
        // All entries are cleared by ApplySnapshot
        CHECK((*wal)->FirstIndex() == 6);
        CHECK((*wal)->LastIndex() == 5);  // last_index = first_index - 1 when empty

        // Old entries should be compacted
        auto term = (*wal)->Term(4);
        CHECK(!term.has_value());
        CHECK(term.error() == StorageErrorCode::Compacted);

        // No entries exist after snapshot (they were all cleared)
        term = (*wal)->Term(6);
        CHECK(!term.has_value());
    }

    TEST_CASE("wal: apply snapshot recovery") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        // Apply snapshot and close
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            std::vector<Entry> entries;
            for (uint64_t i = 1; i <= 20; ++i) {
                entries.push_back(MakeWalEntry(i, 1, "data"));
            }
            CHECK((*wal)->Append(entries));

            Snapshot snap = capnp_util::make<msg::Snapshot>();
            auto snap_builder = capnp_util::builder<msg::Snapshot>(snap);
            auto meta_builder = snap_builder.initMetadata();
            meta_builder.setIndex(10);
            meta_builder.setTerm(1);

            CHECK((*wal)->ApplySnapshot(snap));
            CHECK((*wal)->Close());
        }

        // Reopen and verify
        // ApplySnapshot clears all entries, so last_index = first_index - 1 = 10
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            CHECK((*wal)->FirstIndex() == 11);
            CHECK((*wal)->LastIndex() == 10);
        }
    }

    // ============================================================================
    // ConfState Tests
    // ============================================================================

    TEST_CASE("wal: conf state persistence") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        // Save conf state
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            ConfState cs = capnp_util::make<msg::ConfState>();
            auto cs_builder = capnp_util::builder<msg::ConfState>(cs);
            auto voters = cs_builder.initVoters(3);
            voters.set(0, 1);
            voters.set(1, 2);
            voters.set(2, 3);
            auto learners = cs_builder.initLearners(1);
            learners.set(0, 4);

            auto result = (*wal)->SaveConfState(cs);
            CHECK(result.has_value());
            CHECK((*wal)->Close());
        }

        // Reopen and verify
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            const auto& cs = (*wal)->GetConfState();
            auto cs_reader = capnp_util::reader<msg::ConfState>(cs);
            CHECK(cs_reader.getVoters().size() == 3);
            CHECK(cs_reader.getVoters()[0] == 1);
            CHECK(cs_reader.getVoters()[1] == 2);
            CHECK(cs_reader.getVoters()[2] == 3);
            CHECK(cs_reader.getLearners().size() == 1);
            CHECK(cs_reader.getLearners()[0] == 4);
        }
    }

    // ============================================================================
    // Edge Cases and Boundary Tests
    // ============================================================================

    TEST_CASE("wal: empty wal operations") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Empty WAL should have first_index = 1, last_index = 0
        CHECK((*wal)->FirstIndex() == 1);
        CHECK((*wal)->LastIndex() == 0);

        // Term lookup on empty WAL
        auto term = (*wal)->Term(1);
        CHECK(!term.has_value());

        // ReadEntries on empty WAL
        auto entries = (*wal)->ReadEntries(1, 2, std::nullopt);
        REQUIRE(entries.has_value());
        CHECK(entries->empty());
    }

    TEST_CASE("wal: read entries boundary conditions") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 10; ++i) {
            entries.push_back(MakeWalEntry(i, 1, "data"));
        }
        CHECK((*wal)->Append(entries));

        // Read exact range
        auto result = (*wal)->ReadEntries(1, 11, std::nullopt);
        REQUIRE(result.has_value());
        CHECK(result->size() == 10);

        // Read single entry
        result = (*wal)->ReadEntries(5, 6, std::nullopt);
        REQUIRE(result.has_value());
        CHECK(result->size() == 1);
        CHECK(capnp_util::reader<msg::Entry>((*result)[0]).getIndex() == 5);

        // Read with low == high (empty range)
        result = (*wal)->ReadEntries(5, 5, std::nullopt);
        REQUIRE(result.has_value());
        CHECK(result->empty());

        // Read beyond last index
        result = (*wal)->ReadEntries(1, 100, std::nullopt);
        REQUIRE(result.has_value());
        CHECK(result->size() == 10);
    }

    TEST_CASE("wal: index continuity after append") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Append first batch
        std::vector<Entry> entries1;
        for (uint64_t i = 1; i <= 5; ++i) {
            entries1.push_back(MakeWalEntry(i, 1, "batch1"));
        }
        auto result = (*wal)->Append(entries1);
        CHECK(result.has_value());
        CHECK((*wal)->LastIndex() == 5);

        // Append second batch (must be continuous)
        std::vector<Entry> entries2;
        for (uint64_t i = 6; i <= 10; ++i) {
            entries2.push_back(MakeWalEntry(i, 2, "batch2"));
        }
        result = (*wal)->Append(entries2);
        CHECK(result.has_value());
        CHECK((*wal)->LastIndex() == 10);

        // Verify all entries
        auto read_result = (*wal)->ReadEntries(1, 11, std::nullopt);
        REQUIRE(read_result.has_value());
        CHECK(read_result->size() == 10);

        for (size_t i = 0; i < read_result->size(); ++i) {
            auto reader = capnp_util::reader<msg::Entry>((*read_result)[i]);
            CHECK(reader.getIndex() == i + 1);
        }
    }

    TEST_CASE("wal: term monotonicity") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Append entries with increasing terms
        std::vector<Entry> entries;
        entries.push_back(MakeWalEntry(1, 1, "term1"));
        entries.push_back(MakeWalEntry(2, 1, "term1"));
        entries.push_back(MakeWalEntry(3, 2, "term2"));
        entries.push_back(MakeWalEntry(4, 2, "term2"));
        entries.push_back(MakeWalEntry(5, 3, "term3"));
        CHECK((*wal)->Append(entries));

        // Verify terms
        for (uint64_t i = 1; i <= 5; ++i) {
            auto term = (*wal)->Term(i);
            REQUIRE(term.has_value());
            if (i <= 2) {
                CHECK(*term == 1);
            } else if (i <= 4) {
                CHECK(*term == 2);
            } else {
                CHECK(*term == 3);
            }
        }
    }

    // ============================================================================
    // Multi-Segment Tests
    // ============================================================================

    TEST_CASE("wal: cross segment read") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.segment_size = 500;  // Small segment to force multiple segments
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Append enough entries to span multiple segments
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 100; ++i) {
            entries.push_back(MakeWalEntry(i, 1, "data_" + std::to_string(i)));
        }
        CHECK((*wal)->Append(entries));

        // Read entries that span segments
        auto result = (*wal)->ReadEntries(1, 101, std::nullopt);
        REQUIRE(result.has_value());
        CHECK(result->size() == 100);

        // Verify all entries are correct
        for (size_t i = 0; i < result->size(); ++i) {
            auto reader = capnp_util::reader<msg::Entry>((*result)[i]);
            CHECK(reader.getIndex() == i + 1);
            auto data = reader.getData();
            std::string expected = "data_" + std::to_string(i + 1);
            CHECK(
                std::string(reinterpret_cast<const char*>(data.begin()), data.size()) == expected
            );
        }
    }

    TEST_CASE("wal: segment roll at boundary") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.segment_size = 200;  // Very small segment
        config.write_buffer_size = 50;
        config.sync_on_write = true;

        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            // Append entries one by one to test segment rolling
            for (uint64_t i = 1; i <= 50; ++i) {
                std::vector<Entry> entries;
                entries.push_back(MakeWalEntry(i, 1, "x"));
                auto result = (*wal)->Append(entries);
                CHECK(result.has_value());
            }

            CHECK((*wal)->LastIndex() == 50);
            CHECK((*wal)->Close());
        }

        // Reopen and verify
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            CHECK((*wal)->FirstIndex() == 1);
            CHECK((*wal)->LastIndex() == 50);

            auto result = (*wal)->ReadEntries(1, 51, std::nullopt);
            REQUIRE(result.has_value());
            CHECK(result->size() == 50);
        }
    }

    TEST_CASE("wal: compact removes old segments") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.segment_size = 300;
        config.sync_on_write = true;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Append entries to create multiple segments
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 100; ++i) {
            entries.push_back(MakeWalEntry(i, 1, "data"));
        }
        CHECK((*wal)->Append(entries));

        // Compact to remove old entries
        auto result = (*wal)->Compact(80);
        CHECK(result.has_value());

        CHECK((*wal)->FirstIndex() == 80);
        CHECK((*wal)->LastIndex() == 100);

        // Verify compacted entries are not accessible
        auto term = (*wal)->Term(79);
        CHECK(!term.has_value());
        CHECK(term.error() == StorageErrorCode::Compacted);

        // Verify remaining entries are accessible
        auto read_result = (*wal)->ReadEntries(80, 101, std::nullopt);
        REQUIRE(read_result.has_value());
        CHECK(read_result->size() == 21);
    }

    // ============================================================================
    // WALStorage Interface Tests
    // ============================================================================

    TEST_CASE("wal_storage: compact and entries") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto storage = WALStorage::Open(config);
        REQUIRE(storage.has_value());

        // Append entries
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 20; ++i) {
            entries.push_back(MakeWalEntry(i, 1, "data"));
        }
        CHECK((*storage)->Append(entries));

        // Compact
        auto result = (*storage)->Compact(10);
        CHECK(result.has_value());

        // Verify first/last index
        auto first = (*storage)->FirstIndex();
        REQUIRE(first.has_value());
        CHECK(*first == 10);

        auto last = (*storage)->LastIndex();
        REQUIRE(last.has_value());
        CHECK(*last == 20);

        // Verify entries after compact
        auto entries_result =
            (*storage)->Entries(10, 21, std::nullopt, GetEntriesContext::Empty(false));
        REQUIRE(entries_result.has_value());
        CHECK(entries_result->size() == 11);
    }

    TEST_CASE("wal_storage: apply snapshot") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto storage = WALStorage::Open(config);
        REQUIRE(storage.has_value());

        // Append entries
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 10; ++i) {
            entries.push_back(MakeWalEntry(i, 1, "data"));
        }
        CHECK((*storage)->Append(entries));

        // Apply snapshot
        Snapshot snap = capnp_util::make<msg::Snapshot>();
        auto snap_builder = capnp_util::builder<msg::Snapshot>(snap);
        auto meta_builder = snap_builder.initMetadata();
        meta_builder.setIndex(5);
        meta_builder.setTerm(1);

        auto result = (*storage)->ApplySnapshot(snap);
        CHECK(result.has_value());

        // Verify first index changed
        auto first = (*storage)->FirstIndex();
        REQUIRE(first.has_value());
        CHECK(*first == 6);
    }

    TEST_CASE("wal_storage: snapshot payload survives reopen") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        {
            auto storage = WALStorage::Open(config);
            REQUIRE(storage.has_value());

            auto snap = MakeWalSnapshot(7, 3, {1, 2}, "snapshot-payload");
            CHECK((*storage)->ApplySnapshot(snap));
        }

        auto storage = WALStorage::Open(config);
        REQUIRE(storage.has_value());

        CHECK((*storage)->SnapshotIndex() == 7);

        auto first = (*storage)->FirstIndex();
        REQUIRE(first.has_value());
        CHECK(*first == 8);

        auto term = (*storage)->Term(7);
        REQUIRE(term.has_value());
        CHECK(*term == 3);

        auto snap = (*storage)->GetSnapshot(0, 0);
        REQUIRE(snap.has_value());

        auto snap_reader = capnp_util::reader<msg::Snapshot>(*snap);
        auto snap_meta = snap_reader.getMetadata();
        CHECK(snap_meta.getIndex() == 7);
        CHECK(snap_meta.getTerm() == 3);

        auto voters = snap_meta.getConfState().getVoters();
        REQUIRE(voters.size() == 2);
        CHECK(voters[0] == 1);
        CHECK(voters[1] == 2);

        auto data = snap_reader.getData();
        std::string payload(reinterpret_cast<const char*>(data.begin()), data.size());
        CHECK(payload == "snapshot-payload");

        auto unavailable = (*storage)->GetSnapshot(8, 0);
        CHECK(!unavailable.has_value());
        CHECK(unavailable.error() == StorageErrorCode::SnapshotTemporarilyUnavailable);
    }

    TEST_CASE("wal: load snapshot without snapshot returns unavailable") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        auto snap = (*wal)->LoadSnapshot();
        CHECK(!snap.has_value());
        CHECK(snap.error() == StorageErrorCode::SnapshotTemporarilyUnavailable);
    }

    TEST_CASE("wal_storage: reopen fails when snapshot file is missing") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        {
            auto storage = WALStorage::Open(config);
            REQUIRE(storage.has_value());
            auto snap = MakeWalSnapshot(7, 3, {1}, "snapshot-payload");
            REQUIRE((*storage)->ApplySnapshot(snap));
        }

        std::filesystem::remove(temp_dir.path() / "snapshot");
        auto reopened = WALStorage::Open(config);
        CHECK(!reopened.has_value());
    }

    TEST_CASE("wal_storage: reopen fails when snapshot file is corrupt") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        {
            auto storage = WALStorage::Open(config);
            REQUIRE(storage.has_value());
            auto snap = MakeWalSnapshot(7, 3, {1}, "snapshot-payload");
            REQUIRE((*storage)->ApplySnapshot(snap));
        }

        WriteBytes(temp_dir.path() / "snapshot", std::vector<uint8_t>{'b', 'a', 'd'});
        auto reopened = WALStorage::Open(config);
        CHECK(!reopened.has_value());
    }

    TEST_CASE("wal_storage: reopen fails when snapshot file metadata mismatches") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        {
            auto storage = WALStorage::Open(config);
            REQUIRE(storage.has_value());
            auto snap = MakeWalSnapshot(7, 3, {1}, "snapshot-payload");
            REQUIRE((*storage)->ApplySnapshot(snap));
        }

        auto mismatched = MakeWalSnapshot(8, 3, {1}, "snapshot-payload");
        WriteBytes(temp_dir.path() / "snapshot", capnp_util::toBytes(mismatched));
        auto reopened = WALStorage::Open(config);
        CHECK(!reopened.has_value());
    }

    TEST_CASE("wal_storage: reopen fails when snapshot file is too large") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        {
            auto storage = WALStorage::Open(config);
            REQUIRE(storage.has_value());
            auto snap = MakeWalSnapshot(7, 3, {1}, "snapshot-payload");
            REQUIRE((*storage)->ApplySnapshot(snap));
        }

        std::filesystem::resize_file(
            temp_dir.path() / "snapshot",
            static_cast<uintmax_t>(std::numeric_limits<uint32_t>::max()) + 1
        );
        auto reopened = WALStorage::Open(config);
        CHECK(!reopened.has_value());
    }

    TEST_CASE("wal_storage: apply snapshot failure keeps previous snapshot cached") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        auto storage = WALStorage::Open(config);
        REQUIRE(storage.has_value());

        auto first_snap = MakeWalSnapshot(7, 3, {1}, "old-snapshot");
        REQUIRE((*storage)->ApplySnapshot(first_snap));

        std::filesystem::create_directory(temp_dir.path() / "snapshot.tmp");
        auto next_snap = MakeWalSnapshot(8, 4, {1}, "new-snapshot");
        auto result = (*storage)->ApplySnapshot(next_snap);
        CHECK(!result.has_value());

        CHECK((*storage)->SnapshotIndex() == 7);
        auto cached = (*storage)->GetSnapshot(0, 0);
        REQUIRE(cached.has_value());
        auto cached_reader = capnp_util::reader<msg::Snapshot>(*cached);
        CHECK(cached_reader.getMetadata().getIndex() == 7);
        auto data = cached_reader.getData();
        std::string payload(reinterpret_cast<const char*>(data.begin()), data.size());
        CHECK(payload == "old-snapshot");
    }

    TEST_CASE("wal: snapshot write retries after EINTR") {
        TempDir temp_dir;
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->write_eintr_once = true;

        auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
        CHECK(result.has_value());
        CHECK(!env->write_eintr_once);
    }

    TEST_CASE("wal: apply snapshot fails on write error") {
        TempDir temp_dir;
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->fail_write_once = true;

        auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
        CHECK(!result.has_value());
        CHECK(!env->fail_write_once);
    }

    TEST_CASE("wal: apply snapshot fails on zero-byte write") {
        TempDir temp_dir;
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->zero_write_once = true;

        auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
        CHECK(!result.has_value());
        CHECK(!env->zero_write_once);
    }

    TEST_CASE("wal: apply snapshot fails on snapshot file sync") {
        TempDir temp_dir;
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->fail_sync_snapshot_tmp = true;

        auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
        CHECK(!result.has_value());
    }

    TEST_CASE("wal: apply snapshot fails on snapshot file close") {
        TempDir temp_dir;
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->fail_close_snapshot_tmp = true;

        auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
        CHECK(!result.has_value());
    }

    TEST_CASE("wal: apply snapshot fails on snapshot file rename") {
        TempDir temp_dir;
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->fail_rename_snapshot = true;

        auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
        CHECK(!result.has_value());
    }

    TEST_CASE("wal: apply snapshot fails on snapshot directory operations") {
        SUBCASE("create directories") {
            TempDir temp_dir;
            auto env = std::make_shared<FailingSnapshotEnv>();
            env->fail_create_directories = true;

            auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
            CHECK(!result.has_value());
        }

        SUBCASE("open directory") {
            TempDir temp_dir;
            auto env = std::make_shared<FailingSnapshotEnv>();
            env->fail_open_directory = true;

            auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
            CHECK(!result.has_value());
        }

        SUBCASE("sync directory") {
            TempDir temp_dir;
            auto env = std::make_shared<FailingSnapshotEnv>();
            env->fail_sync_directory = true;

            auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
            CHECK(!result.has_value());
        }
    }

    TEST_CASE("wal: apply snapshot fails on temp file open") {
        TempDir temp_dir;
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->fail_open_snapshot_tmp = true;

        auto result = ApplySnapshotWithEnv(temp_dir.path(), env);
        CHECK(!result.has_value());
    }

    TEST_CASE("wal: snapshot read retries after EINTR") {
        TempDir temp_dir;
        PersistSnapshotForLoad(temp_dir.path());
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->read_eintr_once = true;

        auto result = LoadSnapshotWithEnv(temp_dir.path(), env);
        CHECK(result.has_value());
        CHECK(!env->read_eintr_once);
    }

    TEST_CASE("wal: load snapshot fails on open error") {
        TempDir temp_dir;
        PersistSnapshotForLoad(temp_dir.path());
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->fail_open_snapshot = true;

        auto result = LoadSnapshotWithEnv(temp_dir.path(), env);
        CHECK(!result.has_value());
    }

    TEST_CASE("wal: load snapshot fails on seek errors") {
        SUBCASE("seek end") {
            TempDir temp_dir;
            PersistSnapshotForLoad(temp_dir.path());
            auto env = std::make_shared<FailingSnapshotEnv>();
            env->fail_seek_end = true;

            auto result = LoadSnapshotWithEnv(temp_dir.path(), env);
            CHECK(!result.has_value());
        }

        SUBCASE("seek set") {
            TempDir temp_dir;
            PersistSnapshotForLoad(temp_dir.path());
            auto env = std::make_shared<FailingSnapshotEnv>();
            env->fail_seek_set = true;

            auto result = LoadSnapshotWithEnv(temp_dir.path(), env);
            CHECK(!result.has_value());
        }
    }

    TEST_CASE("wal: load snapshot fails on read error") {
        TempDir temp_dir;
        PersistSnapshotForLoad(temp_dir.path());
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->fail_read_once = true;

        auto result = LoadSnapshotWithEnv(temp_dir.path(), env);
        CHECK(!result.has_value());
        CHECK(!env->fail_read_once);
    }

    TEST_CASE("wal: load snapshot fails on unexpected EOF") {
        TempDir temp_dir;
        PersistSnapshotForLoad(temp_dir.path());
        auto env = std::make_shared<FailingSnapshotEnv>();
        env->eof_read_once = true;

        auto result = LoadSnapshotWithEnv(temp_dir.path(), env);
        CHECK(!result.has_value());
        CHECK(!env->eof_read_once);
    }

    TEST_CASE("wal_storage: all entries") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto storage = WALStorage::Open(config);
        REQUIRE(storage.has_value());

        // Append entries
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 15; ++i) {
            entries.push_back(MakeWalEntry(i, 1, "entry_" + std::to_string(i)));
        }
        CHECK((*storage)->Append(entries));

        // Get all entries
        auto all = (*storage)->AllEntries();
        CHECK(all.size() == 15);

        for (size_t i = 0; i < all.size(); ++i) {
            auto reader = capnp_util::reader<msg::Entry>(all[i]);
            CHECK(reader.getIndex() == i + 1);
        }
    }

    TEST_CASE("wal_storage: is initialized") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto storage = WALStorage::Open(config);
        REQUIRE(storage.has_value());

        // Initially not initialized (no voters)
        CHECK(!(*storage)->IsInitialized());

        // Set conf state with voters
        ConfState cs = capnp_util::make<msg::ConfState>();
        auto cs_builder = capnp_util::builder<msg::ConfState>(cs);
        auto voters = cs_builder.initVoters(1);
        voters.set(0, 1);

        (*storage)->SetConfState(cs);

        // Now should be initialized
        CHECK((*storage)->IsInitialized());
    }

    TEST_CASE("wal_storage: log size bytes") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;
        config.preallocate = false;  // Disable preallocation so file size grows with appends

        auto storage = WALStorage::Open(config);
        REQUIRE(storage.has_value());

        uint64_t initial_size = (*storage)->LogSizeBytes();

        // Append entries
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 100; ++i) {
            entries.push_back(MakeWalEntry(i, 1, std::string(100, 'x')));
        }
        CHECK((*storage)->Append(entries));
        CHECK((*storage)->Sync());

        uint64_t final_size = (*storage)->LogSizeBytes();
        CHECK(final_size > initial_size);
    }

    // ============================================================================
    // Concurrent Access Tests (Basic)
    // ============================================================================

    TEST_CASE("wal: concurrent reads") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = false;

        auto wal = WAL::Open(config);
        REQUIRE(wal.has_value());

        // Append entries
        std::vector<Entry> entries;
        for (uint64_t i = 1; i <= 100; ++i) {
            entries.push_back(MakeWalEntry(i, 1, "data_" + std::to_string(i)));
        }
        CHECK((*wal)->Append(entries));

        // Perform multiple concurrent reads
        std::vector<std::thread> threads;
        std::atomic<int> success_count{0};
        std::atomic<int> failure_count{0};

        for (int t = 0; t < 10; ++t) {
            threads.emplace_back([&wal, &success_count, &failure_count, t]() {
                for (int i = 0; i < 10; ++i) {
                    uint64_t start = (t * 10 + i) % 90 + 1;
                    auto result = (*wal)->ReadEntries(start, start + 10, std::nullopt);
                    // Collect results instead of asserting in thread (doctest assertions
                    // are not thread-safe)
                    if (result.has_value() && result->size() == 10) {
                        success_count++;
                    } else {
                        failure_count++;
                    }
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        // Assert in main thread after all threads complete
        CHECK(failure_count == 0);
        CHECK(success_count == 100);
    }

    // ============================================================================
    // Data Integrity Tests
    // ============================================================================

    TEST_CASE("wal: large entry data integrity") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.sync_on_write = true;

        std::string large_data(64 * 1024, 'A');  // 64KB entry

        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            std::vector<Entry> entries;
            entries.push_back(MakeWalEntry(1, 1, large_data));
            auto result = (*wal)->Append(entries);
            CHECK(result.has_value());
            CHECK((*wal)->Close());
        }

        // Reopen and verify data integrity
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            auto result = (*wal)->ReadEntries(1, 2, std::nullopt);
            REQUIRE(result.has_value());
            REQUIRE(result->size() == 1);

            auto reader = capnp_util::reader<msg::Entry>((*result)[0]);
            auto data = reader.getData();
            CHECK(data.size() == large_data.size());
            CHECK(
                std::string(reinterpret_cast<const char*>(data.begin()), data.size()) == large_data
            );
        }
    }

    TEST_CASE("wal: multiple large entries") {
        TempDir temp_dir;

        WALConfig config;
        config.dir = temp_dir.path();
        config.segment_size = 1024 * 1024;  // 1MB segment
        config.sync_on_write = true;

        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            std::vector<Entry> entries;
            for (uint64_t i = 1; i <= 10; ++i) {
                std::string data(10 * 1024, 'A' + (i % 26));  // 10KB each
                entries.push_back(MakeWalEntry(i, 1, data));
            }
            auto result = (*wal)->Append(entries);
            CHECK(result.has_value());
            CHECK((*wal)->Close());
        }

        // Reopen and verify
        {
            auto wal = WAL::Open(config);
            REQUIRE(wal.has_value());

            CHECK((*wal)->LastIndex() == 10);

            auto result = (*wal)->ReadEntries(1, 11, std::nullopt);
            REQUIRE(result.has_value());
            CHECK(result->size() == 10);

            for (size_t i = 0; i < result->size(); ++i) {
                auto reader = capnp_util::reader<msg::Entry>((*result)[i]);
                auto data = reader.getData();
                CHECK(data.size() == 10 * 1024);
                char expected_char = 'A' + ((i + 1) % 26);
                CHECK(data[0] == static_cast<uint8_t>(expected_char));
            }
        }
    }

    // ============================================================================
    // WAL Index Tests (Additional)
    // ============================================================================

    TEST_CASE("wal_index: empty index operations") {
        WALIndex index;

        CHECK(index.size() == 0);
        CHECK(index.first_index() == 1);  // Default first_index is 1
        CHECK(index.last_index() == 0);   // last_index = first_index - 1 when empty

        auto entry = index.Lookup(1);
        CHECK(!entry.has_value());

        auto term = index.Term(1);
        CHECK(!term.has_value());
    }

    TEST_CASE("wal_index: single entry") {
        WALIndex index;

        index.Insert(1, 1, 100, 50, 5);

        CHECK(index.size() == 1);
        CHECK(index.first_index() == 1);
        CHECK(index.last_index() == 1);

        auto entry = index.Lookup(1);
        REQUIRE(entry.has_value());
        CHECK(entry->segment_id == 1);
        CHECK(entry->offset == 100);
        CHECK(entry->length == 50);
        CHECK(entry->term == 5);
    }

    TEST_CASE("wal_index: truncate all") {
        WALIndex index;

        for (uint64_t i = 1; i <= 10; ++i) {
            index.Insert(i, 1, i * 100, 50, 1);
        }

        CHECK(index.size() == 10);

        // Truncate from index 1 (removes all)
        index.TruncateFrom(1);
        CHECK(index.size() == 0);
        CHECK(index.last_index() == 0);
    }

    TEST_CASE("wal_index: truncate before all") {
        WALIndex index;

        for (uint64_t i = 1; i <= 10; ++i) {
            index.Insert(i, 1, i * 100, 50, 1);
        }

        // Truncate before index 11 (removes all)
        index.TruncateBefore(11);
        CHECK(index.size() == 0);
    }

    // ============================================================================
    // Segment Manager Tests (Additional)
    // ============================================================================

    TEST_CASE("segment: truncate") {
        TempDir temp_dir;
        auto path = temp_dir.path() / "segment-000001.wal";

        auto segment = Segment::Create(path, 1, 1, false, 0);
        REQUIRE(segment.has_value());

        // Write some data
        std::vector<uint8_t> data(1000, 0xAB);
        CHECK((*segment)->Append(data).has_value());
        CHECK((*segment)->Sync().has_value());

        size_t original_offset = (*segment)->write_offset();
        CHECK(original_offset == sizeof(SegmentHeader) + 1000);

        // Truncate to smaller size
        auto result = (*segment)->Truncate(sizeof(SegmentHeader) + 500);
        CHECK(result.has_value());
        CHECK((*segment)->write_offset() == sizeof(SegmentHeader) + 500);
    }

    TEST_CASE("segment: preallocate") {
        TempDir temp_dir;
        auto path = temp_dir.path() / "segment-000001.wal";

        // Create with preallocation
        auto segment = Segment::Create(path, 1, 1, true, 1024 * 1024);
        REQUIRE(segment.has_value());

        // File should be preallocated on Linux (posix_fallocate is only called on Linux)
        auto file_size = std::filesystem::file_size(path);
#ifdef __linux__
        CHECK(file_size >= 1024 * 1024);
#else
        // On non-Linux platforms, preallocation may not be supported
        // Just verify the file exists and write_offset is correct
        CHECK(file_size >= sizeof(SegmentHeader));
#endif

        // Write offset should still be at header
        CHECK((*segment)->write_offset() == sizeof(SegmentHeader));
    }

    // ============================================================================
    // Metadata Store Tests
    // ============================================================================

    TEST_CASE("metadata_store: save and load") {
        TempDir temp_dir;

        MetadataStore store(temp_dir.path());
        auto init_result = store.Initialize();
        CHECK(init_result.has_value());

        // Create metadata
        WALMetadata meta;
        meta.hard_state = capnp_util::make<msg::HardState>();
        auto hs_builder = capnp_util::builder<msg::HardState>(meta.hard_state);
        hs_builder.setTerm(10);
        hs_builder.setVote(5);
        hs_builder.setCommit(100);

        meta.conf_state = capnp_util::make<msg::ConfState>();
        auto cs_builder = capnp_util::builder<msg::ConfState>(meta.conf_state);
        auto voters = cs_builder.initVoters(2);
        voters.set(0, 1);
        voters.set(1, 2);

        meta.first_index = 50;
        meta.snapshot_index = 49;
        meta.snapshot_term = 5;

        // Save
        auto save_result = store.Save(meta);
        CHECK(save_result.has_value());
        CHECK(store.Exists());

        // Load
        auto load_result = store.Load();
        REQUIRE(load_result.has_value());

        auto loaded_hs = capnp_util::reader<msg::HardState>(load_result->hard_state);
        CHECK(loaded_hs.getTerm() == 10);
        CHECK(loaded_hs.getVote() == 5);
        CHECK(loaded_hs.getCommit() == 100);

        auto loaded_cs = capnp_util::reader<msg::ConfState>(load_result->conf_state);
        CHECK(loaded_cs.getVoters().size() == 2);

        CHECK(load_result->first_index == 50);
        CHECK(load_result->snapshot_index == 49);
        CHECK(load_result->snapshot_term == 5);
    }

    TEST_CASE("metadata_store: load after initialize returns default") {
        TempDir temp_dir;

        MetadataStore store(temp_dir.path());
        auto init_result = store.Initialize();
        CHECK(init_result.has_value());

        // Initialize creates a default metadata file
        CHECK(store.Exists());

        // Load should return default metadata
        auto load_result = store.Load();
        REQUIRE(load_result.has_value());
        CHECK(load_result->first_index == 1);
        CHECK(load_result->snapshot_index == 0);
    }

    TEST_CASE("metadata_store: corrupt peer address book returns parse error") {
        TempDir temp_dir;

        MetadataStore store(temp_dir.path());
        CHECK(store.Initialize());

        WALMetadata meta;
        meta.hard_state = capnp_util::make<msg::HardState>();
        meta.conf_state = capnp_util::make<msg::ConfState>();
        meta.peer_addresses = {PeerAddress{1, "127.0.0.1:19101"}};
        REQUIRE(store.Save(meta));

        auto data = ReadBytes(temp_dir.path() / "metadata");
        size_t offset = sizeof(MetadataHeader) + sizeof(MetadataContent);

        uint32_t len = 0;
        REQUIRE(data.size() - offset >= sizeof(len));
        std::memcpy(&len, data.data() + offset, sizeof(len));
        offset += sizeof(len) + len;

        REQUIRE(data.size() - offset >= sizeof(len));
        std::memcpy(&len, data.data() + offset, sizeof(len));
        offset += sizeof(len) + len;

        REQUIRE(data.size() - offset >= sizeof(len));
        uint32_t invalid_book_len = 1;
        std::memcpy(data.data() + offset, &invalid_book_len, sizeof(invalid_book_len));
        offset += sizeof(invalid_book_len);
        data.resize(offset + invalid_book_len);
        data[offset] = 0xff;
        RefreshMetadataCrc(data);
        WriteBytes(temp_dir.path() / "metadata", data);

        auto load_result = store.Load();
        REQUIRE(!load_result.has_value());
        CHECK(load_result.error().Is(StorageErrorCode::PeerAddressBookParseError));
    }

    TEST_CASE("metadata_store: size bytes after save") {
        TempDir temp_dir;

        MetadataStore store(temp_dir.path());
        CHECK(store.Initialize());

        uint64_t initial_size = store.SizeBytes();
        CHECK(initial_size > 0);  // Initialize creates default metadata

        // Save larger metadata
        WALMetadata meta;
        meta.hard_state = capnp_util::make<msg::HardState>();
        auto hs_builder = capnp_util::builder<msg::HardState>(meta.hard_state);
        hs_builder.setTerm(100);
        hs_builder.setVote(50);
        hs_builder.setCommit(1000);

        meta.conf_state = capnp_util::make<msg::ConfState>();
        auto cs_builder = capnp_util::builder<msg::ConfState>(meta.conf_state);
        auto voters = cs_builder.initVoters(5);
        for (int i = 0; i < 5; ++i) {
            voters.set(i, i + 1);
        }

        meta.first_index = 500;
        meta.snapshot_index = 499;
        meta.snapshot_term = 10;

        CHECK(store.Save(meta));

        uint64_t final_size = store.SizeBytes();
        CHECK(final_size > 0);
    }

}  // TEST_SUITE
