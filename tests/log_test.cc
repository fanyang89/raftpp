#include <doctest/doctest.h>

#include "raftpp/core/memory_storage.h"
#include "raftpp/core/raft_log.h"
#include "raftpp/core/types.h"
#include "test_util.h"

using namespace raftpp;

TEST_SUITE_BEGIN("raft_log");

TEST_CASE("raft_log: find conflict") {
    // Test helper to run a single find conflict test case
    auto run_test = [](std::vector<Entry> ents, uint64_t w_conflict) {
        auto previous_entries = Entries(NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3));
        auto store = std::make_unique<MemoryStorage>();
        RaftLog raft_log(DefaultConfig(), std::move(store));
        REQUIRE(raft_log.Append(previous_entries));
        const auto r = raft_log.FindConflict(ents);
        CHECK_EQ(w_conflict, r);
    };

    SUBCASE("no conflict, empty ent") {
        run_test({}, 0);
    }
    SUBCASE("no conflict - exact match") {
        run_test(Entries(NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3)), 0);
    }
    SUBCASE("no conflict - partial match from 2") {
        run_test(Entries(NewEntry(2, 2), NewEntry(3, 3)), 0);
    }
    SUBCASE("no conflict - single match at 3") {
        run_test(Entries(NewEntry(3, 3)), 0);
    }
    SUBCASE("no conflict, but has new entries - from 1") {
        run_test(
            Entries(NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 4)),
            4
        );
    }
    SUBCASE("no conflict, but has new entries - from 2") {
        run_test(Entries(NewEntry(2, 2), NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 4)), 4);
    }
    SUBCASE("no conflict, but has new entries - from 3") {
        run_test(Entries(NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 4)), 4);
    }
    SUBCASE("no conflict, but has new entries - from 4") {
        run_test(Entries(NewEntry(4, 4), NewEntry(5, 4)), 4);
    }
    SUBCASE("conflicts with existing entries - at 1") {
        run_test(Entries(NewEntry(1, 4), NewEntry(2, 4)), 1);
    }
    SUBCASE("conflicts with existing entries - at 2") {
        run_test(Entries(NewEntry(2, 1), NewEntry(3, 4), NewEntry(4, 4)), 2);
    }
    SUBCASE("conflicts with existing entries - at 3") {
        run_test(Entries(NewEntry(3, 1), NewEntry(4, 2), NewEntry(5, 4), NewEntry(6, 4)), 3);
    }
}

TEST_CASE("raft_log: is up-to-date") {
    auto previous_entries = Entries(NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3));

    auto store = std::make_unique<MemoryStorage>();
    RaftLog raft_log(DefaultConfig(), std::move(store));
    REQUIRE(raft_log.Append(previous_entries));

    using TestParam = std::tuple<uint64_t, uint64_t, bool>;
    TestParam test;
    const std::vector<TestParam> tests{
        // greater term, ignore lastIndex
        {raft_log.LastIndex() - 1, 4, true},
        {raft_log.LastIndex(), 4, true},
        {raft_log.LastIndex() + 1, 4, true},
        // smaller term, ignore lastIndex
        {raft_log.LastIndex() - 1, 2, false},
        {raft_log.LastIndex(), 2, false},
        {raft_log.LastIndex() + 1, 2, false},
        // equal term, lager lastIndex wins
        {raft_log.LastIndex() - 1, 3, false},
        {raft_log.LastIndex(), 3, true},
        {raft_log.LastIndex() + 1, 3, true},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [last_index, term, up_to_date] = test;

    const auto r = raft_log.IsUpToDate(last_index, term);
    CHECK_EQ(up_to_date, r);
}

TEST_CASE("raft_log: append") {
    auto run_test = [](std::vector<Entry> entries, uint64_t w_index, std::vector<Entry> w_entries,
                       uint64_t w_unstable) {
        auto previous_entries = Entries(NewEntry(1, 1), NewEntry(2, 2));
        auto store = std::make_unique<MemoryStorage>();
        const auto r = store->MayAppend(previous_entries);
        REQUIRE(r);

        RaftLog raft_log(DefaultConfig(), std::move(store));
        const auto index = raft_log.Append(entries);
        REQUIRE_EQ(index, w_index);

        if (const auto ents =
                raft_log.GetEntries(1, std::nullopt, GetEntriesContext::Empty(false))) {
            CHECK_EQ(ents, w_entries);
        } else {
            FAIL("GetEntries()");
        }
    };

    SUBCASE("empty entries") {
        run_test({}, 2, Entries(NewEntry(1, 1), NewEntry(2, 2)), 3);
    }
    SUBCASE("append new entry") {
        run_test(
            Entries(NewEntry(3, 2)), 3, Entries(NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 2)), 3
        );
    }
    SUBCASE("conflicts with index 1") {
        run_test(Entries(NewEntry(1, 2)), 1, Entries(NewEntry(1, 2)), 1);
    }
    SUBCASE("conflicts with index 2") {
        run_test(
            Entries(NewEntry(2, 3), NewEntry(3, 3)), 3,
            Entries(NewEntry(1, 1), NewEntry(2, 3), NewEntry(3, 3)), 2
        );
    }
}

TEST_CASE("raft_log: compaction side effects") {
    constexpr uint64_t last_index = 1000;
    constexpr uint64_t unstable_index = 750;
    constexpr uint64_t last_term = last_index;

    auto store = std::make_unique<MemoryStorage>();
    const auto store_ptr = store.get();
    for (uint64_t i = 1; i <= unstable_index; i++) {
        const auto r = store->MayAppend(Entries(NewEntry(i, i)));
        CHECK(r);
    }

    RaftLog raft_log(DefaultConfig(), std::move(store));
    for (uint64_t i = unstable_index; i < last_index; ++i) {
        REQUIRE(raft_log.Append(Entries(NewEntry(i + 1, i + 1))));
    }
    CHECK(raft_log.MaybeCommit(last_index, last_term));

    constexpr uint64_t offset = 500;
    REQUIRE(store_ptr->Compact(offset));
    CHECK_EQ(last_index, raft_log.LastIndex());

    for (uint64_t j = offset; j < raft_log.LastIndex(); ++j) {
        REQUIRE_EQ(j, raft_log.Term(j));
        REQUIRE(raft_log.MatchTerm(j, j));
    }

    {
        const auto& unstable_ents = raft_log.unstable().entries();
        REQUIRE_EQ(last_index - unstable_index, unstable_ents.size());
        REQUIRE_EQ(unstable_index + 1, EntryReader(unstable_ents.front()).getIndex());
    }

    auto prev = raft_log.LastIndex();
    REQUIRE(raft_log.Append(Entries(NewEntry(prev + 1, prev + 1))));
    REQUIRE_EQ(prev + 1, raft_log.LastIndex());

    prev = raft_log.LastIndex();
    const auto ents = raft_log.GetEntries(prev, std::nullopt, GetEntriesContext::Empty(false));
    REQUIRE(ents);
    REQUIRE_EQ(1, ents->size());
}

TEST_CASE("raft_log: term with unstable snapshot") {
    constexpr uint64_t storage_snap_idx = 10064;
    constexpr uint64_t unstable_snap_idx = storage_snap_idx + 5;
    auto store = std::make_unique<MemoryStorage>();
    if (const auto r = store->ApplySnapshot(NewSnapshot(storage_snap_idx, 1)); !r) {
        FAIL("ApplySnapshot()");
    }

    RaftLog raft_log(DefaultConfig(), std::move(store));
    REQUIRE(raft_log.Restore(NewSnapshot(unstable_snap_idx, 1)));
    REQUIRE_EQ(raft_log.committed(), unstable_snap_idx);
    REQUIRE_EQ(raft_log.persisted(), storage_snap_idx);

    struct TestParam {
        uint64_t index;
        uint64_t w;
    };

    TestParam test;
    const std::vector<TestParam> tests{
        // cannot get term from storage
        {storage_snap_idx, 0},
        // cannot get term from the gap between storage ents and unstable snapshot
        {storage_snap_idx + 1, 0},
        {unstable_snap_idx - 1, 0},
        // get term from unstable snapshot index
        {unstable_snap_idx, 1},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [index, w] = test;

    const auto r = raft_log.Term(index);
    CHECK_EQ(r, w);
}

TEST_CASE("raft_log: term") {
    constexpr uint64_t offset = 100;
    constexpr uint64_t num = 100;

    auto store = std::make_unique<MemoryStorage>();
    if (const auto r = store->ApplySnapshot(NewSnapshot(offset, 1)); !r) {
        FAIL("ApplySnapshot()");
    }

    RaftLog raft_log(DefaultConfig(), std::move(store));
    for (uint64_t i = 1; i < num; ++i) {
        REQUIRE(raft_log.Append(Entries(NewEntry(offset + i, i))));
    }

    struct TestParam {
        uint64_t index;
        uint64_t w;
    };

    TestParam test;
    const std::vector<TestParam> tests{
        {offset - 1, 0},
        {offset, 1},
        {offset + num / 2, num / 2},
        {offset + num - 1, num - 1},
        {offset + num, 0}
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [index, w] = test;

    const auto term = raft_log.Term(index);
    CHECK_EQ(term, w);
}

TEST_CASE("raft_log: log restore") {
    constexpr uint64_t index = 1000;
    constexpr uint64_t term = 1000;
    auto store = std::make_unique<MemoryStorage>();
    if (const auto r = store->ApplySnapshot(NewSnapshot(index, term)); !r) {
        FAIL("ApplySnapshot()");
    }

    auto entries = Entries(NewEntry(index + 1, term), NewEntry(index + 2, term + 1));

    REQUIRE(store->MayAppend(entries));
    RaftLog raft_log(DefaultConfig(), std::move(store));

    CHECK_EQ(raft_log.AllEntries(), entries);
    CHECK_EQ(index + 1, raft_log.FirstIndex());
    CHECK_EQ(index, raft_log.committed());
    CHECK_EQ(index + 2, raft_log.persisted());
    CHECK_EQ(index + 3, raft_log.unstable().offset());

    CHECK_EQ(term, raft_log.Term(index));
    CHECK_EQ(term, raft_log.Term(index + 1));
    CHECK_EQ(term + 1, raft_log.Term(index + 2));
}

TEST_CASE("raft_log: maybe persist with snapshot") {
    constexpr uint64_t snap_index = 5;
    constexpr uint64_t snap_term = 2;

    // All test cases use empty new_entries
    auto run_test = [snap_index,
                     snap_term](uint64_t stable_index, uint64_t stable_term, uint64_t w_persist) {
        auto store = std::make_unique<MemoryStorage>();
        auto* store_ptr = store.get();
        REQUIRE(store->ApplySnapshot(NewSnapshot(snap_index, snap_term)));
        RaftLog raft_log(DefaultConfig(), std::move(store));
        REQUIRE_EQ(raft_log.persisted(), snap_index);
        // Empty new_entries
        REQUIRE(raft_log.Append({}));

        if (const auto& unstable = raft_log.unstable().entries(); !unstable.empty()) {
            const auto& e = unstable.back();
            auto reader = EntryReader(e);
            raft_log.StableEntries(reader.getIndex(), reader.getTerm());
            CHECK(store_ptr->MayAppend(unstable));
        }

        const bool is_changed = raft_log.persisted() != w_persist;
        CHECK_EQ(raft_log.MaybePersist(stable_index, stable_term), is_changed);
        CHECK_EQ(raft_log.persisted(), w_persist);
    };

    SUBCASE("stable_index > snap_index, same term") {
        run_test(snap_index + 1, snap_term, snap_index);
    }
    SUBCASE("stable_index == snap_index, same term") {
        run_test(snap_index, snap_term, snap_index);
    }
    SUBCASE("stable_index < snap_index, same term") {
        run_test(snap_index - 1, snap_term, snap_index);
    }
    SUBCASE("stable_index > snap_index, higher term") {
        run_test(snap_index + 1, snap_term + 1, snap_index);
    }
    SUBCASE("stable_index == snap_index, higher term") {
        run_test(snap_index, snap_term + 1, snap_index);
    }
    SUBCASE("stable_index < snap_index, higher term") {
        run_test(snap_index - 1, snap_term + 1, snap_index);
    }

    SUBCASE("restore and append") {
        RaftLog raft_log(DefaultConfig(), std::make_unique<MemoryStorage>());
        REQUIRE(raft_log.Restore(NewSnapshot(100, 1)));
        CHECK_EQ(raft_log.unstable().offset(), 101);
        REQUIRE(raft_log.Append(Entries(NewEntry(101, 1))));
        CHECK_EQ(raft_log.Term(101), 1);
        CHECK_FALSE(raft_log.MaybePersist(101, 1));
        REQUIRE(raft_log.Append(Entries(NewEntry(102, 1))));
        CHECK_EQ(raft_log.Term(101), 1);
        CHECK_FALSE(raft_log.MaybePersist(102, 1));
    }
}

TEST_CASE("raft_log: unstable entries") {
    SUBCASE("unstable = 3, w_entries = empty") {
        // When unstable offset is 3, all entries are in storage
        auto store = std::make_unique<MemoryStorage>();
        auto previous_ents = Entries(NewEntry(1, 1), NewEntry(2, 2));
        REQUIRE(store->Append(previous_ents));  // Store all entries

        RaftLog raft_log(DefaultConfig(), std::move(store));
        // No unstable entries

        const auto& ents = raft_log.unstable().entries();
        if (!ents.empty()) {
            const auto& e = ents.back();
            auto reader = EntryReader(e);
            raft_log.StableEntries(reader.getIndex(), reader.getTerm());
        }
        CHECK(ents.empty());

        CHECK_EQ(3, raft_log.unstable().offset());
    }

    SUBCASE("unstable = 1, w_entries = all") {
        // When unstable offset is 1, all entries are unstable
        auto store = std::make_unique<MemoryStorage>();
        // No entries in storage

        RaftLog raft_log(DefaultConfig(), std::move(store));
        auto previous_ents = Entries(NewEntry(1, 1), NewEntry(2, 2));
        REQUIRE(raft_log.Append(previous_ents));

        const auto& ents = raft_log.unstable().entries();
        REQUIRE_EQ(2, ents.size());
        if (!ents.empty()) {
            const auto& e = ents.back();
            auto reader = EntryReader(e);
            raft_log.StableEntries(reader.getIndex(), reader.getTerm());
        }
        CHECK_EQ(ents, previous_ents);

        CHECK_EQ(3, raft_log.unstable().offset());
    }
}

TEST_CASE("raft_log: has next entries and next entries") {
    // Test with range-based expected entries
    // w_entries_range = std::nullopt means no entries expected
    // w_entries_range = {start, end} means entries[start..end] expected
    struct TestParam {
        uint64_t applied = 0;
        uint64_t persisted = 0;
        uint64_t committed = 0;
        std::optional<std::pair<size_t, size_t>> w_entries_range;
    };

    const std::vector<TestParam> tests{
        {0, 3, 3, std::nullopt}, {0, 3, 4, std::nullopt}, {0, 4, 6, {{0, 1}}},
        {0, 6, 4, {{0, 1}}},     {0, 5, 5, {{0, 2}}},     {0, 5, 7, {{0, 2}}},
        {0, 7, 5, {{0, 2}}},     {3, 4, 3, std::nullopt}, {3, 5, 5, {{0, 2}}},
        {3, 6, 7, {{0, 3}}},     {3, 7, 6, {{0, 3}}},     {4, 5, 5, {{1, 2}}},
        {4, 5, 7, {{1, 2}}},     {4, 7, 5, {{1, 2}}},     {4, 7, 7, {{1, 4}}},
        {5, 5, 5, std::nullopt}, {5, 7, 7, {{2, 4}}},     {7, 7, 7, std::nullopt},
    };

    TestParam test;
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [applied, persisted, committed, w_entries_range] = test;

    // Create entries fresh for each test case
    auto ents = Entries(NewEntry(4, 1), NewEntry(5, 1), NewEntry(6, 1), NewEntry(7, 1));

    auto store = std::make_unique<MemoryStorage>();
    auto* store_ptr = store.get();
    CHECK(store->ApplySnapshot(NewSnapshot(3, 1)));

    RaftLog raft_log(DefaultConfig(), std::move(store));
    REQUIRE(raft_log.Append(ents));

    const auto& unstable = raft_log.unstable().entries();
    if (!unstable.empty()) {
        const auto& e = unstable.back();
        auto reader = EntryReader(e);
        raft_log.StableEntries(reader.getIndex(), reader.getTerm());
        REQUIRE(store_ptr->Append(unstable));
    }

    std::ignore = raft_log.MaybePersist(persisted, 1);
    CHECK_EQ(persisted, raft_log.persisted());

    std::ignore = raft_log.MaybeCommit(committed, 1);
    CHECK_EQ(committed, raft_log.committed());

    raft_log.AppliedTo(applied);

    CHECK_EQ(w_entries_range.has_value(), raft_log.HasNextEntries());
    const auto next_entries = raft_log.NextEntries({});
    if (w_entries_range.has_value()) {
        const auto [start, end] = *w_entries_range;
        auto expected = EntriesSlice(ents, start, end);
        CHECK_EQ(next_entries, expected);
    } else {
        CHECK_FALSE(next_entries.has_value());
    }
}

TEST_CASE("raft_log: has next entries and next entries, 2") {
    // Range-based expected entries: nullopt means no entries, {start, end} means entries[start..end]
    // Using 7 as "all" since entries has 7 elements
    struct TestParam {
        uint64_t applied = 0;
        uint64_t persisted = 0;
        uint64_t committed = 0;
        uint64_t limit = 0;
        std::optional<std::pair<size_t, size_t>> w_entries_range;
    };

    constexpr uint64_t UNLIMITED = std::numeric_limits<uint32_t>::max();
    const std::vector<TestParam> tests{
        {0, 3, 3, 0, std::nullopt},
        {0, 3, 4, 0, std::nullopt},
        {0, 3, 4, UNLIMITED, {{0, 1}}},
        {0, 4, 6, 0, {{0, 1}}},
        {0, 4, 6, 2, {{0, 3}}},
        {0, 4, 6, 6, {{0, 3}}},
        {0, 4, 10, 0, {{0, 1}}},
        {0, 4, 10, 2, {{0, 3}}},
        {0, 4, 10, 6, {{0, 7}}},  // all entries
        {0, 4, 10, 7, {{0, 7}}},  // all entries
        {0, 6, 4, 0, {{0, 1}}},
        {0, 6, 4, UNLIMITED, {{0, 1}}},
        {0, 5, 5, 0, {{0, 2}}},
        {3, 4, 3, UNLIMITED, std::nullopt},
        {3, 5, 5, UNLIMITED, {{0, 2}}},
        {3, 6, 7, UNLIMITED, {{0, 4}}},
        {3, 7, 6, UNLIMITED, {{0, 3}}},
        {4, 5, 5, UNLIMITED, {{1, 2}}},
        {4, 5, 7, UNLIMITED, {{1, 4}}},
        {4, 5, 9, UNLIMITED, {{1, 6}}},
        {4, 5, 10, UNLIMITED, {{1, 7}}},  // to end
        {4, 7, 5, UNLIMITED, {{1, 2}}},
        {4, 7, 7, 0, {{1, 4}}},
        {5, 5, 5, 0, std::nullopt},
        {5, 7, 7, UNLIMITED, {{2, 4}}},
        {7, 7, 7, UNLIMITED, std::nullopt},
        // test applied can be bigger than `persisted + limit`(when limit is changed)
        {8, 6, 8, 0, std::nullopt},
    };

    TestParam test;
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [applied, persisted, committed, limit, w_entries_range] = test;

    // Create entries fresh for each test case
    auto ents = Entries(
        NewEntry(4, 1), NewEntry(5, 1), NewEntry(6, 1), NewEntry(7, 1), NewEntry(8, 1),
        NewEntry(9, 1), NewEntry(10, 1)
    );

    auto store = std::make_unique<MemoryStorage>();
    auto* store_ptr = store.get();
    CHECK(store->ApplySnapshot(NewSnapshot(3, 1)));

    RaftLog raft_log(DefaultConfig(), std::move(store));
    raft_log.max_apply_unpersisted_log_limit() = limit;
    REQUIRE(raft_log.Append(ents));

    const auto& unstable = raft_log.unstable().entries();
    if (!unstable.empty()) {
        const auto& e = unstable.back();
        auto reader = EntryReader(e);
        raft_log.StableEntries(reader.getIndex(), reader.getTerm());
        REQUIRE(store_ptr->Append(unstable));
    }

    std::ignore = raft_log.MaybePersist(persisted, 1);
    CHECK_EQ(persisted, raft_log.persisted());

    std::ignore = raft_log.MaybeCommit(committed, 1);
    CHECK_EQ(committed, raft_log.committed());

    raft_log.AppliedTo(applied);

    CHECK_EQ(w_entries_range.has_value(), raft_log.HasNextEntries());
    const auto next_entries = raft_log.NextEntries({});
    if (w_entries_range.has_value()) {
        const auto [start, end] = *w_entries_range;
        auto expected = EntriesSlice(ents, start, end);
        CHECK_EQ(next_entries, expected);
    } else {
        CHECK_FALSE(next_entries.has_value());
    }
}

TEST_CASE("raft_log: slice") {
    constexpr uint64_t offset = 100;
    constexpr uint64_t num = 100;
    constexpr uint64_t last = offset + num;
    constexpr uint64_t half = offset + num / 2;
    auto half_e = NewEntry(half, half);
    const auto half_e_size = EntrySize(half_e);

    auto store = std::make_unique<MemoryStorage>();
    CHECK(store->ApplySnapshot(NewSnapshot(offset, 0)));
    for (uint64_t i = 1; i < num / 2; ++i) {
        REQUIRE(store->Append(Entries(NewEntry(offset + i, offset + i))));
    }

    RaftLog raft_log(DefaultConfig(), std::move(store));
    for (uint64_t i = num / 2; i < num; ++i) {
        REQUIRE(raft_log.Append(Entries(NewEntry(offset + i, offset + i))));
    }

    // Helper to create expected entries for a range [from, to)
    auto expected_entries = [](uint64_t from, uint64_t to) {
        std::vector<Entry> entries;
        for (uint64_t i = from; i < to; ++i) {
            entries.push_back(NewEntry(i, i));
        }
        return entries;
    };

    // Test parameters without Entry vectors - use w_from/w_to to generate expected entries
    // w_from == w_to means empty expected entries
    struct TestParam {
        uint64_t from = 0;
        uint64_t to = 0;
        uint64_t limit = 0;
        uint64_t w_from = 0;  // Expected entries range start
        uint64_t w_to = 0;    // Expected entries range end
        bool w_panic = false;
        size_t test_index = 0;
    };

    TestParam test;
    const std::vector<TestParam> tests{
        // test no limit
        {offset - 1, offset + 1, NO_LIMIT, 0, 0, false},  // empty
        {offset, offset + 1, NO_LIMIT, 0, 0, false},      // empty
        {half - 1, half + 1, NO_LIMIT, half - 1, half + 1, false},
        {half, half + 1, NO_LIMIT, half, half + 1, false},
        {last - 1, last, NO_LIMIT, last - 1, last, false},
        {last, last + 1, NO_LIMIT, 0, 0, true},
        // test limit
        {half - 1, half + 1, 0, half - 1, half, false},  // limit = 0 means 1 entry
        {half - 1, half + 1, half_e_size + 1, half - 1, half, false},
        {half - 2, half + 1, half_e_size + 1, half - 2, half - 1, false},
        {half - 1, half + 1, half_e_size * 2, half - 1, half + 1, false},
        {half - 1, half + 2, half_e_size * 3, half - 1, half + 2, false},
        {half, half + 2, half_e_size, half, half + 1, false},
        {half, half + 2, half_e_size * 2, half, half + 2, false},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA_WITH_INDEX(test, tests);
    const auto& [from, to, limit, w_from, w_to, w_panic, test_index] = test;

    auto slice_result = raft_log.Slice(from, to, limit, GetEntriesContext::Empty(false));

    if (w_panic) {
        if (slice_result || !slice_result.error().Is<FatalError>()) {
            FAIL("expected error");
        }
        return;
    }

    // compacted
    if (from <= offset) {
        if (slice_result || !slice_result.error().Is(StorageErrorCode::Compacted)) {
            FAIL("Expected Compacted error");
        }
        return;
    }

    auto w = expected_entries(w_from, w_to);
    REQUIRE_EQ(slice_result, w);
}

size_t ents_size(const std::vector<Entry>& ents) {
    return std::accumulate(
        ents.begin(), ents.end(), 0,
        [](const size_t previous, const Entry& entry) { return previous + EntrySize(entry); }
    );
}

TEST_CASE("raft_log: scan") {
    constexpr auto offset = 47;
    constexpr auto num = 20;
    constexpr auto last = offset + num;
    constexpr auto half = offset + num / 2;
    auto entries = [](uint64_t from, uint64_t to) {
        std::vector<Entry> ents;
        for (uint64_t i = from; i < to; ++i) {
            ents.emplace_back(NewEntry(i, i));
        }
        return ents;
    };

    auto entry_size = ents_size(entries(half, half + 1));

    auto store = std::make_unique<MemoryStorage>();
    REQUIRE(store->ApplySnapshot(NewSnapshot(offset, 0)));
    REQUIRE(store->Append(entries(offset + 1, half)));
    RaftLog raft_log(DefaultConfig(), std::move(store));
    REQUIRE(raft_log.Append(entries(half, last)));

    size_t page_size = 0;
    const std::vector<size_t> page_sizes{0, 1, 10, 100, entry_size, entry_size + 1};
    DOCTEST_VALUE_PARAMETERIZED_DATA(page_size, page_sizes);

    // Test that scan() returns the same entries as slice(), on all inputs.
    for (auto lo = offset + 1; lo <= last; ++lo) {
        for (auto hi = lo; hi <= last; ++hi) {
            std::vector<Entry> got;
            REQUIRE(raft_log.Scan(
                lo, hi, page_size, GetEntriesContext::Empty(false),
                [&got, page_size](const std::vector<Entry>& ents) {
                    const bool ok = ents.size() == 1 || ents_size(ents) < page_size;
                    CHECK(ok);
                    for (const auto& e : ents) {
                        got.push_back(CloneEntry(e));
                    }
                    return true;
                }
            ));
            auto want = raft_log.Slice(lo, hi, {}, GetEntriesContext::Empty(false));
            REQUIRE(want);
            CHECK_EQ(want, got);
        }
    }

    // Test that the callback early return.
    int iters = 0;
    REQUIRE(raft_log.Scan(
        offset + 1, half, 0, GetEntriesContext::Empty(false), [&iters](const std::vector<Entry>&) {
            iters++;
            if (iters == 2) {
                return false;
            }
            return true;
        }
    ));
    REQUIRE_EQ(iters, 2);

    // Test that we max out the limit, and not just always return a single entry.
    // NB: this test works only because the requested range length is even.
    REQUIRE(raft_log.Scan(
        offset + 1, offset + 11, entry_size * 2, GetEntriesContext::Empty(false),
        [entry_size](const std::vector<Entry>& ents) {
            CHECK_EQ(ents.size(), 2);
            CHECK_EQ(entry_size * 2, ents_size(ents));
            return true;
        }
    ));
}

TEST_CASE("raft_log: maybe append") {
    constexpr uint64_t last_index = 3;
    constexpr uint64_t last_term = 3;
    constexpr uint64_t commit = 1;
    constexpr uint64_t persist = 3;

    // Helper to create previous entries
    auto make_previous_ents = []() {
        return Entries(NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3));
    };

    // Test parameters using (index, term) pairs for entries instead of Entry objects
    struct TestParam {
        uint64_t log_term = 0;
        uint64_t index = 0;
        uint64_t committed = 0;
        std::vector<std::pair<uint64_t, uint64_t>> ent_specs;  // (index, term) pairs
        std::optional<uint64_t> w_last_index;
        uint64_t w_commit = 0;
        uint64_t w_persist = 0;
        bool w_panic = false;
    };

    // Helper to create entries from specs
    auto make_entries = [](const std::vector<std::pair<uint64_t, uint64_t>>& specs) {
        std::vector<Entry> result;
        for (const auto& [idx, term] : specs) {
            result.push_back(NewEntry(idx, term));
        }
        return result;
    };

    // Run a single test case
    auto run_test = [&](const TestParam& test, const char* name) {
        DOCTEST_SUBCASE(name) {
            const auto& [log_term, index, committed, ent_specs, w_last_index, w_commit, w_persist, w_panic] =
                test;
            auto ents = make_entries(ent_specs);

            auto store = std::make_unique<MemoryStorage>();
            RaftLog raft_log(DefaultConfig(), std::move(store));
            REQUIRE(raft_log.Append(make_previous_ents()));
            raft_log.committed() = commit;
            raft_log.persisted() = persist;

            const auto r = raft_log.MaybeAppend(index, log_term, committed, ents);
            if (!r) {
                if (!w_panic) {
                    FAIL("unexpected error: ", r.error());
                    return;
                }
                REQUIRE(r.error().Is<FatalError>());
                return;
            }

            auto success = r->term_matched;
            auto g_last_index = r->last_index;

            const uint64_t g_committed = raft_log.committed();
            const uint64_t g_persisted = raft_log.persisted();

            if (success) {
                REQUIRE_EQ(g_last_index, w_last_index);
            } else {
                // MaybeAppend() failed: term mismatch
                REQUIRE(!w_last_index.has_value());
            }

            REQUIRE_EQ(g_committed, w_commit);
            REQUIRE_EQ(g_persisted, w_persist);

            if (success && !ents.empty()) {
                const auto from = raft_log.LastIndex() + 1 - ents.size();
                const auto to = raft_log.LastIndex() + 1;
                const auto g_ents =
                    raft_log.Slice(from, to, std::nullopt, GetEntriesContext::Empty(false));
                REQUIRE_EQ(g_ents, ents);
            }
        }
    };

    // not match: term is different
    run_test(
        {last_term - 1,
         last_index,
         last_index,
         {{last_index + 1, 4}},
         std::nullopt,
         commit,
         persist,
         false},
        "term is different"
    );

    // not match: index out of bound
    run_test(
        {last_term,
         last_index + 1,
         last_index,
         {{last_index + 2, 4}},
         std::nullopt,
         commit,
         persist,
         false},
        "index out of bound"
    );

    // match with the last existing entry
    run_test(
        {last_term, last_index, last_index, {}, last_index, last_index, persist, false},
        "match with last entry, empty ents"
    );

    // do not increase commit higher than last_new_i
    run_test(
        {last_term, last_index, last_index + 1, {}, last_index, last_index, persist, false},
        "do not increase commit higher than last_new_i (1)"
    );

    // commit up to the commit in the message
    run_test(
        {last_term, last_index, last_index - 1, {}, last_index, last_index - 1, persist, false},
        "commit up to the commit in the message"
    );

    // commit do not decrease
    run_test(
        {last_term, last_index, 0, {}, last_index, commit, persist, false},
        "commit do not decrease (1)"
    );
    run_test({0, 0, last_index, {}, 0, commit, persist, false}, "commit do not decrease (2)");

    // append with new entry
    run_test(
        {last_term,
         last_index,
         last_index,
         {{last_index + 1, 4}},
         last_index + 1,
         last_index,
         persist,
         false},
        "append one entry"
    );
    run_test(
        {last_term,
         last_index,
         last_index + 1,
         {{last_index + 1, 4}},
         last_index + 1,
         last_index + 1,
         persist,
         false},
        "append one entry and commit"
    );

    // do not increase commit higher than last_new_i
    run_test(
        {last_term,
         last_index,
         last_index + 2,
         {{last_index + 1, 4}},
         last_index + 1,
         last_index + 1,
         persist,
         false},
        "do not increase commit higher than last_new_i (2)"
    );
    run_test(
        {last_term,
         last_index,
         last_index + 2,
         {{last_index + 1, 4}, {last_index + 2, 4}},
         last_index + 2,
         last_index + 2,
         persist,
         false},
        "append two entries"
    );

    // match with the entry in the middle
    run_test(
        {last_term - 1,
         last_index - 1,
         last_index,
         {{last_index, 4}},
         last_index,
         last_index,
         std::min(last_index - 1, persist),
         false},
        "match in middle (1)"
    );
    run_test(
        {last_term - 2,
         last_index - 2,
         last_index,
         {{last_index - 1, 4}},
         last_index - 1,
         last_index - 1,
         std::min(last_index - 2, persist),
         false},
        "match in middle (2)"
    );

    // conflict with existing committed entry
    run_test(
        {last_term - 3,
         last_index - 3,
         last_index,
         {{last_index - 2, 4}},
         last_index - 2,
         last_index - 2,
         std::min(last_index - 3, persist),
         true},
        "conflict with committed entry"
    );
    run_test(
        {last_term - 2,
         last_index - 2,
         last_index,
         {{last_index - 1, 4}, {last_index, 4}},
         last_index,
         last_index,
         std::min(last_index - 2, persist),
         false},
        "overwrite middle entries (1)"
    );
    run_test(
        {last_term - 2,
         last_index - 2,
         last_index + 2,
         {{last_index - 1, last_term - 1}, {last_index, 4}, {last_index + 1, 4}},
         last_index + 1,
         last_index + 1,
         std::min(last_index - 1, persist),
         false},
        "overwrite middle entries (2)"
    );
}

TEST_CASE("raft_log: commit to") {
    constexpr auto previous_commit = 2;

    // Helper to create previous entries
    auto make_previous_ents = []() {
        return Entries(NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3));
    };

    struct TestParam {
        uint64_t commit = 0;
        uint64_t w_commit = 0;
        bool w_panic = false;
    };

    TestParam test;
    const std::vector tests{
        TestParam{3, 3, false},
        TestParam{1, 2, false},  // never decrease
        TestParam{4, 0, true},   // commit out of range -> panic
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [commit, w_commit, w_panic] = test;

    auto store = std::make_unique<MemoryStorage>();
    RaftLog raft_log(DefaultConfig(), std::move(store));
    REQUIRE(raft_log.Append(make_previous_ents()));
    raft_log.committed() = previous_commit;

    const auto r = raft_log.CommitTo(commit);
    if (w_panic) {
        if (r || !r.error().Is<FatalError>()) {
            FAIL("expected fatal error");
        }
        return;
    }

    CHECK_EQ(raft_log.committed(), w_commit);
}

TEST_CASE("raft_log: compaction") {
    struct TestParam {
        uint64_t index = 0;
        std::vector<uint64_t> compact;
        std::vector<size_t> w_left;
        bool should_panic = false;
    };

    TestParam test;
    const std::vector tests{
        // out of upper bound
        TestParam{1000, {1001}, {0}, true},
        TestParam{
            1000,
            {300, 500, 800, 900},
            {700, 500, 200, 100},
            false,
        },
        // out of lower bound
        TestParam{1000, {300, 299}, {700, 700}, false},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [index, compact, w_left, should_panic] = test;

    auto store = std::make_unique<MemoryStorage>();
    auto* store_ptr = store.get();
    for (size_t i = 1; i < index; ++i) {
        REQUIRE(store->Append(Entries(NewEntry(i, 0))));
    }

    RaftLog raft_log(DefaultConfig(), std::move(store));
    std::ignore = raft_log.MaybeCommit(index - 1, 0);
    const auto committed = raft_log.committed();
    raft_log.AppliedTo(committed);

    for (size_t i = 0; i < compact.size(); ++i) {
        const auto idx = compact[i];
        const auto r = store_ptr->Compact(idx);
        if (should_panic) {
            if (r || !r.error().Is<FatalError>()) {
                FAIL("expected fatal error");
            }
            return;
        }
        const auto l = raft_log.AllEntries().size();
        REQUIRE_EQ(l, w_left[i]);
    }
}

class RaftLogDebug : public RaftLog {
  public:
    using RaftLog::RaftLog;

    [[nodiscard]] Result<void> MustCheckOutOfBounds(const uint64_t low, const uint64_t high) const {
        return RaftLog::MustCheckOutOfBounds(low, high);
    }
};

TEST_CASE("raft_log: is out of bounds") {
    constexpr uint64_t offset = 100;
    constexpr uint64_t num = 100;
    constexpr uint64_t first = offset + 1;

    auto store = std::make_unique<MemoryStorage>();
    REQUIRE(store->ApplySnapshot(NewSnapshot(offset, 0)));

    RaftLogDebug raft_log(DefaultConfig(), std::move(store));
    for (size_t i = 1; i <= num; ++i) {
        REQUIRE(raft_log.Append(Entries(NewEntry(i + offset, 0))));
    }

    struct TestParam {
        uint64_t lo = 0;
        uint64_t hi = 0;
        bool w_panic = false;
        bool w_err_compacted = false;
    };

    TestParam test;
    const std::vector<TestParam> tests{
        {first - 2, first + 1, false, true},
        {first - 1, first + 1, false, true},
        {first, first, false, false},
        {first + num / 2, first + num / 2, false, false},
        {first + num - 1, first + num - 1, false, false},
        {first + num, first + num, false, false},
        {first + num, first + num + 1, true, false},
        {first + num + 1, first + num + 1, true, false},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [lo, hi, w_panic, w_err_compacted] = test;

    const auto r = raft_log.MustCheckOutOfBounds(lo, hi);
    if (w_panic) {
        if (r || !r.error().Is<FatalError>()) {
            FAIL("expected fatal error");
        }
        return;
    }

    if (w_err_compacted) {
        REQUIRE(r.error().Is(StorageErrorCode::Compacted));
    }
}

TEST_CASE("raft_log: restore snapshot") {
    auto store = std::make_unique<MemoryStorage>();
    auto* store_ptr = store.get();
    REQUIRE(store->ApplySnapshot(NewSnapshot(100, 1)));

    RaftLog raft_log(DefaultConfig(), std::move(store));
    REQUIRE_EQ(raft_log.committed(), 100);
    REQUIRE_EQ(raft_log.persisted(), 100);

    REQUIRE(raft_log.Restore(NewSnapshot(200, 1)));
    REQUIRE_EQ(raft_log.committed(), 200);
    REQUIRE_EQ(raft_log.persisted(), 100);

    for (uint64_t i = 201; i < 210; ++i) {
        REQUIRE(raft_log.Append(Entries(NewEntry(i, 1))));
    }

    REQUIRE(store_ptr->ApplySnapshot(NewSnapshot(200, 1)));
    raft_log.StableSnapshot(200);

    const auto& unstable = raft_log.unstable().entries();
    raft_log.StableEntries(209, 1);
    REQUIRE(store_ptr->Append(CloneEntries(unstable)));
    REQUIRE(raft_log.MaybePersist(209, 1));
    CHECK_EQ(raft_log.persisted(), 209);

    REQUIRE(raft_log.Restore(NewSnapshot(205, 1)));
    REQUIRE_EQ(raft_log.committed(), 205);
    REQUIRE_EQ(raft_log.persisted(), 200);

    const auto r = raft_log.Restore(NewSnapshot(204, 1));
    CHECK_FALSE(r);
}

TEST_SUITE_END();
