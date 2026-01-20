#include <numeric>
#include <optional>
#include <vector>

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include "raftpp/core/types.h"
#include "raftpp/core/unstable_log.h"
#include "raftpp/core/util.h"
#include "test_util.h"

using namespace raftpp;

TEST_SUITE_BEGIN("unstable_log");

TEST_CASE("unstable_log: maybe first index") {
    // Test cases: has_entry, offset, has_snapshot, w_ok, w_index
    struct TestSpec {
        bool has_entry;
        uint64_t offset;
        bool has_snapshot;
        bool w_ok;
        uint64_t w_index;
    };

    std::vector<TestSpec> tests{
        // NoSnapshot
        {true, 5, false, false, 0},
        {false, 0, false, false, 0},
        // HasSnapshot
        {true, 5, true, true, 5},
        {false, 5, true, true, 5},
    };

    for (const auto& test : tests) {
        size_t entries_size = 0;
        std::vector<Entry> entries;
        if (test.has_entry) {
            Entry ent = NewEntry(5, 1);
            entries_size = EntryApproximateSize(ent);
            entries.push_back(std::move(ent));
        }

        std::optional<Snapshot> snapshot;
        if (test.has_snapshot) {
            snapshot = NewSnapshot(4, 1);
        }

        const Unstable u(std::move(entries), entries_size, test.offset, std::move(snapshot));
        if (const auto index = u.MaybeFirstIndex(); index) {
            CHECK_EQ(test.w_index, index);
        } else {
            CHECK_FALSE(test.w_ok);
        }
    }
}

TEST_CASE("unstable_log: maybe last index") {
    // Test cases: has_entry, offset, has_snapshot, w_ok, w_index
    struct TestSpec {
        bool has_entry;
        uint64_t offset;
        bool has_snapshot;
        bool w_ok;
        uint64_t w_index;
    };

    std::vector<TestSpec> tests{
        {true, 5, false, true, 5},
        {true, 5, true, true, 5},
        // last in snapshot
        {false, 5, true, true, 4},
        // empty unstable
        {false, 0, false, false, 0},
    };

    for (const auto& test : tests) {
        size_t entries_size = 0;
        std::vector<Entry> entries;

        if (test.has_entry) {
            Entry ent = NewEntry(5, 1);
            entries_size = EntryApproximateSize(ent);
            entries.push_back(std::move(ent));
        }

        std::optional<Snapshot> snapshot;
        if (test.has_snapshot) {
            snapshot = NewSnapshot(4, 1);
        }

        Unstable u(std::move(entries), entries_size, test.offset, std::move(snapshot));
        const auto index = u.MaybeLastIndex();
        if (index) {
            CHECK_EQ(test.w_index, index);
        } else {
            CHECK_FALSE(test.w_ok);
        }
    }
}

TEST_CASE("unstable_log: maybe term") {
    // Test cases: has_entry, offset, has_snapshot, index, w_ok, w_term
    struct TestSpec {
        bool has_entry;
        uint64_t offset;
        bool has_snapshot;
        uint64_t index;
        bool w_ok;
        uint64_t w_term;
    };

    std::vector<TestSpec> tests{
        {true, 5, false, 5, true, 1},
        {true, 5, false, 6, false, 0},
        {true, 5, false, 4, false, 0},
        {true, 5, true, 5, true, 1},
        {true, 5, true, 6, false, 0},
        // term from snapshot
        {true, 5, true, 4, true, 1},
        {true, 5, true, 3, false, 0},
        {false, 5, true, 5, false, 0},
        {false, 5, true, 4, true, 1},
        {false, 0, false, 5, false, 0},
    };

    for (const auto& test : tests) {
        size_t entries_size = 0;
        std::vector<Entry> entries;

        if (test.has_entry) {
            Entry ent = NewEntry(5, 1);
            entries_size = EntryApproximateSize(ent);
            entries.push_back(std::move(ent));
        }

        std::optional<Snapshot> snapshot;
        if (test.has_snapshot) {
            snapshot = NewSnapshot(4, 1);
        }

        Unstable u(std::move(entries), entries_size, test.offset, std::move(snapshot));
        const auto term = u.MaybeTerm(test.index);
        if (term) {
            CHECK_EQ(test.w_term, term);
        } else {
            CHECK_FALSE(test.w_ok);
        }
    }
}

TEST_CASE("unstable_log: restore") {
    std::vector<Entry> init_entries;
    Entry init_ent = NewEntry(5, 1);
    size_t entries_size = EntryApproximateSize(init_ent);
    init_entries.push_back(std::move(init_ent));
    Unstable u(std::move(init_entries), entries_size, 5, NewSnapshot(4, 1));

    const auto s = NewSnapshot(6, 2);
    u.Restore(s);

    CHECK_EQ(u.offset(), capnp_util::reader<msg::Snapshot>(s).getMetadata().getIndex() + 1);
    CHECK(u.entries().empty());
    CHECK_EQ(u.entries_size(), 0);
    // Snapshot comparison: check metadata matches
    REQUIRE(u.snapshot().has_value());
    auto u_snap_reader = capnp_util::reader<msg::Snapshot>(*u.snapshot());
    auto s_reader = capnp_util::reader<msg::Snapshot>(s);
    CHECK_EQ(u_snap_reader.getMetadata().getIndex(), s_reader.getMetadata().getIndex());
    CHECK_EQ(u_snap_reader.getMetadata().getTerm(), s_reader.getMetadata().getTerm());
}

TEST_CASE("unstable_log: stable snapshot and entries") {
    std::vector<Entry> ents;
    ents.push_back(NewEntry(5, 1));
    ents.push_back(NewEntry(5, 2));
    ents.push_back(NewEntry(6, 3));

    size_t entries_size = 0;
    for (const auto& ent : ents) {
        entries_size += capnp_util::toBytes(ent).size();
    }

    std::vector<Entry> ents_copy;
    for (const auto& ent : ents) {
        ents_copy.push_back(CloneEntry(ent));
    }

    Unstable u(std::move(ents), entries_size, 5, NewSnapshot(4, 1));
    CHECK_EQ(ents_copy, u.entries());

    u.StableSnapshot(4);
    u.StableEntries(6, 3);
    CHECK(u.entries().empty());
    CHECK_EQ(u.entries_size(), 0);
    CHECK_EQ(u.offset(), 7);
}

TEST_CASE("unstable_log: truncate and append") {
    // Test case specifications using entry index/term pairs
    struct EntrySpec {
        uint64_t index;
        uint64_t term;
    };

    struct TestSpec {
        std::vector<EntrySpec> entries;
        uint64_t offset;
        std::vector<EntrySpec> to_append;
        uint64_t w_offset;
        std::vector<EntrySpec> w_entries;
    };

    std::vector<TestSpec> tests{
        // append to existing entries
        TestSpec{{{5, 1}}, 5, {{6, 1}, {7, 1}}, 5, {{5, 1}, {6, 1}, {7, 1}}},
        // replace unstable entries
        TestSpec{{{5, 1}}, 5, {{5, 2}, {6, 2}}, 5, {{5, 2}, {6, 2}}},
        TestSpec{{{5, 1}}, 5, {{4, 2}, {5, 2}, {6, 2}}, 4, {{4, 2}, {5, 2}, {6, 2}}},
        // truncate existing entries and append
        TestSpec{{{5, 1}, {6, 1}, {7, 1}}, 5, {{6, 2}}, 5, {{5, 1}, {6, 2}}},
        TestSpec{
            {{5, 1}, {6, 1}, {7, 1}}, 5, {{7, 2}, {8, 2}}, 5, {{5, 1}, {6, 1}, {7, 2}, {8, 2}}
        },
    };

    for (const auto& test : tests) {
        // Build entries from specs
        std::vector<Entry> entries;
        for (const auto& spec : test.entries) {
            entries.push_back(NewEntry(spec.index, spec.term));
        }

        const size_t entries_size = std::accumulate(
            entries.begin(), entries.end(), size_t{0},
            [](size_t acc, const Entry& ent) { return acc + EntryApproximateSize(ent); }
        );

        Unstable u(std::move(entries), entries_size, test.offset, std::nullopt);

        // Build to_append entries
        std::vector<Entry> to_append;
        for (const auto& spec : test.to_append) {
            to_append.push_back(NewEntry(spec.index, spec.term));
        }
        u.TruncateAndAppend(to_append);

        CHECK_EQ(u.offset(), test.w_offset);

        // Build expected entries for comparison
        std::vector<Entry> w_entries;
        for (const auto& spec : test.w_entries) {
            w_entries.push_back(NewEntry(spec.index, spec.term));
        }
        CHECK_EQ(u.entries(), w_entries);

        const size_t w_entries_size = std::accumulate(
            w_entries.begin(), w_entries.end(), size_t{0},
            [](size_t acc, const Entry& ent) { return acc + EntryApproximateSize(ent); }
        );
        CHECK_EQ(u.entries_size(), w_entries_size);
    }
}

TEST_SUITE_END();
