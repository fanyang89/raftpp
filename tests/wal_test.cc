#include "raftpp/raftor/wal/wal.h"

#include <filesystem>
#include <random>

#include <doctest/doctest.h>
#include <kj/array.h>

#include "raftpp/raftor/wal/crc32c.h"
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
            std::ignore = (*segment)->Append(data);
            std::ignore = (*segment)->Sync();
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
        std::ignore = (*wal)->Append(entries);

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
            std::ignore = (*wal)->Append(entries);
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
        std::ignore = (*wal)->Append(entries);

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
        std::ignore = (*wal)->Append(entries);

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

}  // TEST_SUITE
