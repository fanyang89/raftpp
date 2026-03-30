#include "raftpp/core/storage.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>

#include "harness/test_util.h"
#include "raftpp/core/memory_storage.h"
#include "test_util.h"

using namespace raftpp;

namespace {

template <typename T>
size_t size_of(const T& m) {
    return capnp_util::toBytes(m).size();
}

}  // namespace

template <>
struct fmt::formatter<std::vector<Entry>> : formatter<std::string_view> {
    static format_context::iterator format(
        const std::vector<Entry>& values, const format_context& ctx
    ) {
        std::vector<std::string> s;
        s.reserve(values.size());
        for (const auto& v : values) {
            auto reader = capnp_util::reader<msg::Entry>(v);
            auto data = reader.getData();
            s.emplace_back(fmt::format(
                "{{index={} term={} data_size={}}}", reader.getIndex(), reader.getTerm(),
                data.size()
            ));
        }
        return fmt::format_to(ctx.out(), "[\n{}\n]", fmt::join(s, ",\n"));
    }
};

TEST_SUITE_BEGIN("storage");

TEST_CASE("storage: term") {
    // Use helper to create entries without copies
    auto MakeEntries = []() {
        std::vector<Entry> v;
        v.push_back(NewEntry(3, 3));
        v.push_back(NewEntry(4, 4));
        v.push_back(NewEntry(5, 5));
        return v;
    };

    // Test specs: (index, expected_term or error, is_error)
    struct TestSpec {
        uint64_t idx;
        uint64_t wTerm;
        bool is_error;
        StorageErrorCode error_code;
    };

    std::vector<TestSpec> tests{
        {2, 0, true, StorageErrorCode::Compacted},
        {3, 3, false, {}},
        {4, 4, false, {}},
        {5, 5, false, {}},
        {6, 0, true, StorageErrorCode::Unavailable}
    };

    for (size_t ti = 0; ti < tests.size(); ++ti) {
        CAPTURE(ti);
        const auto& t = tests[ti];

        MemoryStorage storage;
        storage.SetEntries(MakeEntries());
        const auto term = storage.Term(t.idx);

        if (t.is_error) {
            CHECK(!term.has_value());
            CHECK(term.error().Is(t.error_code));
        } else {
            CHECK(term.has_value());
            CHECK_EQ(*term, t.wTerm);
        }
    }
}

TEST_CASE("storage: entries") {
    // Helper to create entries
    auto MakeEntries = []() {
        std::vector<Entry> v;
        v.push_back(NewEntry(3, 3));
        v.push_back(NewEntry(4, 4));
        v.push_back(NewEntry(5, 5));
        v.push_back(NewEntry(6, 6));
        return v;
    };

    // Pre-compute sizes for the size-based tests
    auto ents = MakeEntries();
    size_t size1 = size_of(ents[1]);  // Entry(4,4)
    size_t size2 = size_of(ents[2]);  // Entry(5,5)
    size_t size3 = size_of(ents[3]);  // Entry(6,6)

    // Test specs: (lo, hi, maxSize, expected_entries as (index,term) pairs, is_error, error_code)
    struct TestSpec {
        uint64_t lo;
        uint64_t hi;
        uint64_t maxSize;
        std::vector<std::pair<uint64_t, uint64_t>> wEntries;  // expected (index, term) pairs
        bool is_error;
        StorageErrorCode error_code;
    };

    std::vector<TestSpec> tests{
        {2, 6, std::numeric_limits<uint64_t>::max(), {}, true, StorageErrorCode::Compacted},
        {3, 4, std::numeric_limits<uint64_t>::max(), {{3, 3}}, false, {}},
        {4, 5, std::numeric_limits<uint64_t>::max(), {{4, 4}}, false, {}},
        {4, 6, std::numeric_limits<uint64_t>::max(), {{4, 4}, {5, 5}}, false, {}},
        {4, 7, std::numeric_limits<uint64_t>::max(), {{4, 4}, {5, 5}, {6, 6}}, false, {}},
        // even if maxsize is zero, the first entry should be returned
        {4, 7, 0, {{4, 4}}, false, {}},
        // limit to 2
        {4, 7, size1 + size2, {{4, 4}, {5, 5}}, false, {}},
        {4, 7, size1 + size2 + size3 / 2, {{4, 4}, {5, 5}}, false, {}},
        {4, 7, size1 + size2 + size3 - 1, {{4, 4}, {5, 5}}, false, {}},
        // all
        {4, 7, size1 + size2 + size3, {{4, 4}, {5, 5}, {6, 6}}, false, {}},
    };

    for (size_t ti = 0; ti < tests.size(); ++ti) {
        CAPTURE(ti);
        const auto& t = tests[ti];

        MemoryStorage storage;
        storage.SetEntries(MakeEntries());
        const auto result = storage.Entries(t.lo, t.hi, t.maxSize, GetEntriesContext::Empty(false));

        if (t.is_error) {
            CHECK(!result.has_value());
            CHECK(result.error().Is(t.error_code));
        } else {
            REQUIRE(result.has_value());
            // Create expected entries
            std::vector<Entry> wEntries;
            for (const auto& [idx, term] : t.wEntries) {
                wEntries.push_back(NewEntry(idx, term));
            }
            CHECK(raftpp::operator==(*result, wEntries));
        }
    }
}

TEST_CASE("storage: last index") {
    auto MakeEntries = []() {
        std::vector<Entry> v;
        v.push_back(NewEntry(3, 3));
        v.push_back(NewEntry(4, 4));
        v.push_back(NewEntry(5, 5));
        return v;
    };

    MemoryStorage storage;
    storage.SetEntries(MakeEntries());

    auto result = storage.LastIndex();
    CHECK_EQ(5, result);

    std::vector<Entry> new_entries;
    new_entries.push_back(NewEntry(6, 5));
    std::ignore = storage.Append(new_entries);
    result = storage.LastIndex();
    CHECK_EQ(6, result);
}

TEST_CASE("storage: first index") {
    auto MakeEntries = []() {
        std::vector<Entry> v;
        v.push_back(NewEntry(3, 3));
        v.push_back(NewEntry(4, 4));
        v.push_back(NewEntry(5, 5));
        return v;
    };

    MemoryStorage storage;
    storage.SetEntries(MakeEntries());
    CHECK_EQ(3, storage.FirstIndex());

    std::ignore = storage.Compact(4);
    CHECK_EQ(4, storage.FirstIndex());
}

TEST_CASE("storage: compact") {
    auto MakeEntries = []() {
        std::vector<Entry> v;
        v.push_back(NewEntry(3, 3));
        v.push_back(NewEntry(4, 4));
        v.push_back(NewEntry(5, 5));
        return v;
    };

    struct TestSpec {
        uint64_t idx;
        uint64_t wIndex;
        uint64_t wTerm;
        uint64_t wLen;
    };

    std::vector<TestSpec> tests{
        {2, 3, 3, 3},
        {3, 3, 3, 3},
        {4, 4, 4, 2},
        {5, 5, 5, 1},
    };

    for (size_t ti = 0; ti < tests.size(); ++ti) {
        CAPTURE(ti);
        const auto& t = tests[ti];

        MemoryStorage storage;
        storage.SetEntries(MakeEntries());
        std::ignore = storage.Compact(t.idx);

        uint64_t index = 0;
        if (const auto r = storage.FirstIndex(); r) {
            index = *r;
        } else {
            FAIL("FirstIndex()");
        }
        REQUIRE_EQ(t.wIndex, index);

        uint64_t term = 0;
        if (const auto r = storage.Entries(index, index + 1, 1, GetEntriesContext::Empty(false))) {
            if (!r->empty()) {
                term = capnp_util::reader<msg::Entry>(r->front()).getTerm();
            }
        }
        REQUIRE_EQ(t.wTerm, term);

        uint64_t last = 0;
        if (const auto r = storage.LastIndex(); r) {
            last = *r;
        } else {
            FAIL("LastIndex()");
        }

        size_t len;
        if (const auto r = storage.Entries(
                index, last + 1, std::numeric_limits<uint64_t>::max(),
                GetEntriesContext::Empty(false)
            );
            r) {
            len = r->size();
        } else {
            FAIL("Entries()");
        }
        REQUIRE_EQ(t.wLen, len);
    }
}

TEST_CASE("storage: create snapshot") {
    auto MakeEntries = []() {
        std::vector<Entry> v;
        v.push_back(NewEntry(3, 3));
        v.push_back(NewEntry(4, 4));
        v.push_back(NewEntry(5, 5));
        return v;
    };

    const std::vector<uint64_t> nodes{1, 2, 3};
    ConfState conf_state = capnp_util::make<msg::ConfState>();
    auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
    auto voters_builder = conf_builder.initVoters(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        voters_builder.set(i, nodes[i]);
    }

    // Test case specs: idx, is_error, expected_index, expected_term (for non-error), wIndex
    struct TestSpec {
        uint64_t idx;
        bool is_error;
        uint64_t expected_snap_idx;
        uint64_t expected_snap_term;
        uint64_t wIndex;
    };

    std::vector<TestSpec> tests{
        {4, false, 4, 4, 0},
        {5, false, 5, 5, 5},
        {5, false, 6, 5, 6},
        {5, true, 0, 0, 6},  // error case: SnapshotTemporarilyUnavailable
    };

    for (const auto& test : tests) {
        MemoryStorage storage;
        storage.SetEntries(MakeEntries());

        RaftState raft_state;
        raft_state.hard_state = MakeHardState(test.idx, 0, test.idx);
        raft_state.conf_state = CloneConfState(conf_state);
        storage.SetRaftState(std::move(raft_state));

        if (test.is_error) {
            storage.TriggerSnapshotUnavailable();
        }

        const auto result = storage.GetSnapshot(test.wIndex, 0);

        if (test.is_error) {
            CHECK(!result.has_value());
            CHECK(result.error().Is(StorageErrorCode::SnapshotTemporarilyUnavailable));
        } else {
            CHECK(result.has_value());
            auto snap_reader = capnp_util::reader<msg::Snapshot>(*result);
            CHECK_EQ(snap_reader.getMetadata().getIndex(), test.expected_snap_idx);
            CHECK_EQ(snap_reader.getMetadata().getTerm(), test.expected_snap_term);
        }
    }
}

TEST_CASE("storage: append") {
    // Test case spec: entries to append, expected entries (empty = error case)
    struct TestSpec {
        std::vector<std::pair<uint64_t, uint64_t>> entries;  // (index, term) to append
        std::vector<std::pair<uint64_t, uint64_t>>
            wEntries;  // expected (index, term), empty = error
        bool is_error;
    };

    std::vector<TestSpec> tests{
        // Same entries as initial
        {{{3, 3}, {4, 4}, {5, 5}}, {{3, 3}, {4, 4}, {5, 5}}, false},
        // Override terms of entries 4 and 5
        {{{3, 3}, {4, 6}, {5, 6}}, {{3, 3}, {4, 6}, {5, 6}}, false},
        // Append new entry
        {{{3, 3}, {4, 4}, {5, 5}, {6, 5}}, {{3, 3}, {4, 4}, {5, 5}, {6, 5}}, false},
        // Gap in entries - error case
        {{{2, 3}, {3, 3}, {4, 5}}, {}, true},
        // Truncate and replace
        {{{4, 5}}, {{3, 3}, {4, 5}}, false},
        // Simple append at end
        {{{6, 6}}, {{3, 3}, {4, 4}, {5, 5}, {6, 6}}, false},
    };

    for (size_t ti = 0; ti < tests.size(); ++ti) {
        CAPTURE(ti);
        const auto& test = tests[ti];

        // Create initial entries
        std::vector<Entry> ents;
        ents.push_back(NewEntry(3, 3));
        ents.push_back(NewEntry(4, 4));
        ents.push_back(NewEntry(5, 5));

        MemoryStorage storage;
        storage.SetEntries(ents);

        // Create entries to append
        std::vector<Entry> entries;
        for (const auto& [idx, term] : test.entries) {
            entries.push_back(NewEntry(idx, term));
        }

        if (test.is_error) {
            const auto r = storage.MayAppend(entries);
            CHECK(!r.has_value());
        } else {
            std::ignore = storage.Append(entries);

            // Create expected entries
            std::vector<Entry> wEntries;
            for (const auto& [idx, term] : test.wEntries) {
                wEntries.push_back(NewEntry(idx, term));
            }

            CHECK(raftpp::operator==(wEntries, storage.AllEntries()));
        }
    }
}

TEST_CASE("storage: apply snapshot") {
    const std::vector<uint64_t> nodes{1, 2, 3};
    MemoryStorage storage;

    // Apply snapshot successfully
    auto snap = NewSnapshot(4, 4, nodes);
    if (auto r = storage.ApplySnapshot(snap); !r) {
        FAIL("ApplySnapshot()");
    }

    // Apply snapshot fails due to StorageError::SnapshotOutOfDate
    snap = NewSnapshot(3, 3, nodes);
    if (auto r = storage.ApplySnapshot(snap); r) {
        FAIL("ApplySnapshot()");
    }
}

TEST_SUITE_END();
