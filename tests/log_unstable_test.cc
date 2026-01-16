#include <numeric>
#include <optional>
#include <vector>

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include "raftpp/raftpp.pb.h"
#include "raftpp/unstable_log.h"
#include "raftpp/util.h"
#include "test_util.h"

using namespace raftpp;

struct LogUnstableTestParams {
    std::optional<Entry> ent;
    uint64_t offset = 0;
    std::optional<Snapshot> snapshot;
    bool w_ok = false;
    uint64_t w_index = 0;

    friend std::ostream& operator<<(std::ostream& os, const LogUnstableTestParams& param) {
        return os << (param.snapshot.has_value() ? "has entry" : "don't have entry");
    }
};

TEST_SUITE_BEGIN("unstable_log");

TEST_CASE("unstable_log: maybe first index") {
    LogUnstableTestParams params;
    std::list<LogUnstableTestParams> tests{
        // NoSnapshot
        {NewEntry(5, 1), 5, {}, false, 0},
        {{}, 0, {}, false, 0},
        // HasSnapshot
        {NewEntry(5, 1), 5, NewSnapshot(4, 1), true, 5},
        {{}, 5, NewSnapshot(4, 1), true, 5},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(params, tests);
    const auto& [ent, offset, snapshot, w_ok, w_index] = params;

    size_t entries_size = 0;
    std::vector<Entry> entries;
    if (ent) {
        entries.emplace_back(*ent);
        entries_size += EntryApproximateSize(*ent);
    }

    const Unstable u(entries, entries_size, offset, snapshot);
    if (const auto index = u.MaybeFirstIndex(); index) {
        CHECK_EQ(w_index, index);
    } else {
        CHECK_FALSE(w_ok);
    }
}

TEST_CASE("unstable_log: maybe last index") {
    struct TestParam {
        std::optional<Entry> entry;
        uint64_t offset = 0;
        std::optional<Snapshot> snapshot;
        bool w_ok = false;
        uint64_t w_index = 0;
    };

    TestParam test;
    std::vector<TestParam> tests{
        {NewEntry(5, 1), 5, std::nullopt, true, 5},
        {NewEntry(5, 1), 5, NewSnapshot(4, 1), true, 5},
        // last in snapshot
        {std::nullopt, 5, NewSnapshot(4, 1), true, 4},
        // empty unstable
        {std::nullopt, 0, std::nullopt, false, 0},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [ent, offset, snapshot, w_ok, w_index] = test;

    size_t entries_size = 0;
    std::vector<Entry> entries;

    if (ent) {
        entries_size = EntryApproximateSize(*ent);
        entries.emplace_back(*ent);
    }

    Unstable u(entries, entries_size, offset, snapshot);
    const auto index = u.MaybeLastIndex();
    if (index) {
        CHECK_EQ(w_index, index);
    } else {
        CHECK_FALSE(w_ok);
    }
}

TEST_CASE("unstable_log: maybe term") {
    struct TestParam {
        std::optional<Entry> entry;
        uint64_t offset = 0;
        std::optional<Snapshot> snapshot;
        uint64_t index = 0;
        bool w_ok = false;
        uint64_t w_term = 0;
    };

    TestParam test;
    std::vector<TestParam> tests{
        {NewEntry(5, 1), 5, std::nullopt, 5, true, 1},
        {NewEntry(5, 1), 5, std::nullopt, 6, false, 0},
        {NewEntry(5, 1), 5, std::nullopt, 4, false, 0},
        {
            NewEntry(5, 1),
            5,
            NewSnapshot(4, 1),
            5,
            true,
            1,
        },
        {
            NewEntry(5, 1),
            5,
            NewSnapshot(4, 1),
            6,
            false,
            0,
        },
        // term from snapshot
        {
            NewEntry(5, 1),
            5,
            NewSnapshot(4, 1),
            4,
            true,
            1,
        },
        {
            NewEntry(5, 1),
            5,
            NewSnapshot(4, 1),
            3,
            false,
            0,
        },
        {std::nullopt, 5, NewSnapshot(4, 1), 5, false, 0},
        {std::nullopt, 5, NewSnapshot(4, 1), 4, true, 1},
        {std::nullopt, 0, std::nullopt, 5, false, 0},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [ent, offset, snapshot, index, w_ok, w_term] = test;

    size_t entries_size = 0;
    std::vector<Entry> entries;

    if (ent) {
        entries_size = EntryApproximateSize(*ent);
        entries.emplace_back(*ent);
    }

    Unstable u(entries, entries_size, offset, snapshot);
    const auto term = u.MaybeTerm(index);
    if (term) {
        CHECK_EQ(w_term, term);
    } else {
        CHECK_FALSE(w_ok);
    }
}

TEST_CASE("unstable_log: restore") {
    Unstable u({NewEntry(5, 1)}, EntryApproximateSize(NewEntry(5, 1)), 5, {NewSnapshot(4, 1)});

    const auto s = NewSnapshot(6, 2);
    u.Restore(s);

    CHECK_EQ(u.offset(), s.metadata().index() + 1);
    CHECK(u.entries().empty());
    CHECK_EQ(u.entries_size(), 0);
    CHECK_EQ(u.snapshot(), s);
}

TEST_CASE("unstable_log: stable snapshot and entries") {
    std::vector<Entry> ents{
        NewEntry(5, 1),
        NewEntry(5, 2),
        NewEntry(6, 3),
    };

    size_t entries_size =
        std::accumulate(ents.begin(), ents.end(), 0, [](const size_t acc, const Entry& ent) {
            return acc + ent.ByteSizeLong();
        });

    Unstable u(ents, entries_size, 5, {NewSnapshot(4, 1)});
    CHECK_EQ(ents, u.entries());

    u.StableSnapshot(4);
    u.StableEntries(6, 3);
    CHECK(u.entries().empty());
    CHECK_EQ(u.entries_size(), 0);
    CHECK_EQ(u.offset(), 7);
}

TEST_CASE("unstable_log: truncate and append") {
    struct TestParam {
        std::vector<Entry> entries;
        uint64_t offset = 0;
        std::optional<Snapshot> snapshot;
        std::vector<Entry> to_append;
        uint64_t w_offset = 0;
        std::vector<Entry> w_entries;
    };

    TestParam test;
    std::vector<TestParam> tests{
        TestParam{
            {NewEntry(5, 1)},
            5,
            std::nullopt,
            {NewEntry(6, 1), NewEntry(7, 1)},
            5,
            {NewEntry(5, 1), NewEntry(6, 1), NewEntry(7, 1)}
        },
        // replace to unstable entries
        TestParam{
            {NewEntry(5, 1)},
            5,
            std::nullopt,
            {NewEntry(5, 2), NewEntry(6, 2)},
            5,
            {NewEntry(5, 2), NewEntry(6, 2)}
        },
        TestParam{
            {NewEntry(5, 1)},
            5,
            std::nullopt,
            {NewEntry(4, 2), NewEntry(5, 2), NewEntry(6, 2)},
            4,
            {NewEntry(4, 2), NewEntry(5, 2), NewEntry(6, 2)}
        },
        // truncate existing entries and append
        TestParam{
            {NewEntry(5, 1), NewEntry(6, 1), NewEntry(7, 1)},
            5,
            std::nullopt,
            {NewEntry(6, 2)},
            5,
            {NewEntry(5, 1), NewEntry(6, 2)}
        },
        TestParam{
            {NewEntry(5, 1), NewEntry(6, 1), NewEntry(7, 1)},
            5,
            std::nullopt,
            {NewEntry(7, 2), NewEntry(8, 2)},
            5,
            {
                NewEntry(5, 1),
                NewEntry(6, 1),
                NewEntry(7, 2),
                NewEntry(8, 2),
            }
        },
    };

    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [entries, offset, snapshot, to_append, w_offset, w_entries] = test;

    const size_t entries_size =
        std::accumulate(entries.begin(), entries.end(), 0, [](const size_t acc, const Entry& ent) {
            return acc + EntryApproximateSize(ent);
        });
    Unstable u(entries, entries_size, offset, snapshot);
    u.TruncateAndAppend(to_append);
    CHECK_EQ(u.offset(), w_offset);
    CHECK_EQ(u.entries(), w_entries);

    const size_t w_entries_size = std::accumulate(
        w_entries.begin(), w_entries.end(), 0,
        [](const size_t acc, const Entry& ent) { return acc + EntryApproximateSize(ent); }
    );
    CHECK_EQ(u.entries_size(), w_entries_size);
}

TEST_SUITE_END();
