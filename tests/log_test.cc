#include <variant>

#include <doctest/doctest.h>

#include "raftpp/memory_storage.h"
#include "raftpp/raft_log.h"
#include "raftpp/raftpp.pb.h"
#include "test_util.h"

using namespace raftpp;

TEST_SUITE_BEGIN("raft_log");

TEST_CASE("raft_log: find conflict") {
    const std::vector previous_entries{
        NewEntry(1, 1),
        NewEntry(2, 2),
        NewEntry(3, 3),
    };

    struct TestParam {
        std::vector<Entry> entries;
        uint64_t w_conflict = 0;
    };

    TestParam test;
    const std::vector<TestParam> tests{
        // no conflict, empty ent
        {{}, 0},
        // no conflict
        {{NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3)}, 0},
        {{NewEntry(2, 2), NewEntry(3, 3)}, 0},
        {{NewEntry(3, 3)}, 0},
        // no conflict, but has new entries
        {{NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3), NewEntry(4, 4),
          NewEntry(5, 4)},
         4},
        {{NewEntry(2, 2), NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 4)}, 4},
        {{NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 4)}, 4},
        {{NewEntry(4, 4), NewEntry(5, 4)}, 4},
        // conflicts with existing entries
        {{NewEntry(1, 4), NewEntry(2, 4)}, 1},
        {{NewEntry(2, 1), NewEntry(3, 4), NewEntry(4, 4)}, 2},
        {{NewEntry(3, 1), NewEntry(4, 2), NewEntry(5, 4), NewEntry(6, 4)}, 3},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [ents, w_conflict] = test;

    auto store = std::make_unique<MemoryStorage>();
    RaftLog raft_log(DefaultConfig(), std::move(store));
    raft_log.Append(previous_entries);

    const auto r = raft_log.FindConflict(ents);
    CHECK_EQ(w_conflict, r);
}

TEST_CASE("raft_log: is up-to-date") {
    const std::vector previous_entries{
        NewEntry(1, 1),
        NewEntry(2, 2),
        NewEntry(3, 3),
    };

    auto store = std::make_unique<MemoryStorage>();
    RaftLog raft_log(DefaultConfig(), std::move(store));
    raft_log.Append(previous_entries);

    using TestParam = std::tuple<uint64_t, uint64_t, bool>;
    TestParam test;
    std::vector<TestParam> tests{
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
    const std::vector previous_entries{
        NewEntry(1, 1),
        NewEntry(2, 2),
    };

    struct TestParam {
        std::vector<Entry> entries;
        uint64_t w_index = 0;
        std::vector<Entry> w_entries;
        uint64_t w_unstable = 0;
    };

    TestParam test;
    const std::vector<TestParam> tests{
        {{}, 2, {NewEntry(1, 1), NewEntry(2, 2)}, 3},
        {
            {NewEntry(3, 2)},
            3,
            {NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 2)},
            3,
        },
        // conflicts with index 1
        {{NewEntry(1, 2)}, 1, {NewEntry(1, 2)}, 1},
        // conflicts with index 2
        {
            {NewEntry(2, 3), NewEntry(3, 3)},
            3,
            {NewEntry(1, 1), NewEntry(2, 3), NewEntry(3, 3)},
            2,
        },
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [entries, w_index, w_entries, w_unstable] = test;

    auto store = std::make_unique<MemoryStorage>();
    const auto r = store->MayAppend(previous_entries);
    REQUIRE(r);

    RaftLog raft_log(DefaultConfig(), std::move(store));
    const auto index = raft_log.Append(entries);
    REQUIRE_EQ(index, w_index);

    if (const auto ents = raft_log.GetEntries(
            1, std::nullopt, GetEntriesContext::Empty(false)
        )) {
        CHECK_EQ(ents, w_entries);
    } else {
        FAIL("GetEntries()");
    }
}

TEST_CASE("raft_log: compaction side effects") {
    const uint64_t last_index = 1000;
    const uint64_t unstable_index = 750;
    const uint64_t last_term = last_index;

    auto store = std::make_unique<MemoryStorage>();
    auto store_ptr = store.get();
    for (uint64_t i = 1; i <= unstable_index; i++) {
        const auto r = store->MayAppend({NewEntry(i, i)});
        CHECK(r);
    }

    RaftLog raft_log(DefaultConfig(), std::move(store));
    for (uint64_t i = unstable_index; i < last_index; ++i) {
        raft_log.Append({NewEntry(i + 1, i + 1)});
    }
    CHECK(raft_log.MaybeCommit(last_index, last_term));

    const uint64_t offset = 500;
    store_ptr->Compact(offset);
    CHECK_EQ(last_index, raft_log.LastIndex());

    for (uint64_t j = offset; j < raft_log.LastIndex(); ++j) {
        REQUIRE_EQ(j, raft_log.Term(j));
        REQUIRE(raft_log.MatchTerm(j, j));
    }

    {
        const auto unstable_ents = raft_log.unstable().entries();
        REQUIRE_EQ(last_index - unstable_index, unstable_ents.size());
        REQUIRE_EQ(unstable_index + 1, unstable_ents.front().index());
    }

    auto prev = raft_log.LastIndex();
    raft_log.Append({NewEntry(prev + 1, prev + 1)});
    REQUIRE_EQ(prev + 1, raft_log.LastIndex());

    prev = raft_log.LastIndex();
    const auto ents = raft_log.GetEntries(
        prev, std::nullopt, GetEntriesContext::Empty(false)
    );
    REQUIRE(ents);
    REQUIRE_EQ(1, ents->size());
}

TEST_CASE("raft_log: term with unstable snapshot") {
    constexpr uint64_t storage_snap_idx = 10064;
    constexpr uint64_t unstable_snap_idx = storage_snap_idx + 5;
    auto store = std::make_unique<MemoryStorage>();
    if (const auto r = store->ApplySnapshot(NewSnapshot(storage_snap_idx, 1));
        !r) {
        FAIL("ApplySnapshot()");
    }

    RaftLog raft_log(DefaultConfig(), std::move(store));
    raft_log.Restore(NewSnapshot(unstable_snap_idx, 1));
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
        raft_log.Append({NewEntry(offset + i, i)});
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

    const std::vector entries{
        NewEntry(index + 1, term),
        NewEntry(index + 2, term + 1),
    };

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
    {
        constexpr uint64_t snap_index = 5;
        constexpr uint64_t snap_term = 2;

        struct TestParam {
            uint64_t stable_index = 0;
            uint64_t stable_term = 0;
            std::vector<Entry> new_entries;
            uint64_t w_persist = 0;
        };

        TestParam test;
        const std::vector<TestParam> tests{
            {snap_index + 1, snap_term, {}, snap_index},
            {snap_index, snap_term, {}, snap_index},
            {snap_index - 1, snap_term, {}, snap_index},
            {snap_index + 1, snap_term + 1, {}, snap_index},
            {snap_index, snap_term + 1, {}, snap_index},
            {snap_index - 1, snap_term + 1, {}, snap_index},
        };
        DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
        const auto [stable_index, stable_term, new_entries, w_persist] = test;
        auto store = std::make_unique<MemoryStorage>();
        auto* store_ptr = store.get();
        REQUIRE(store->ApplySnapshot(NewSnapshot(snap_index, snap_term)));
        RaftLog raft_log(DefaultConfig(), std::move(store));
        REQUIRE_EQ(raft_log.persisted(), snap_index);
        raft_log.Append(new_entries);

        if (const auto& unstable = raft_log.unstable().entries();
            !unstable.empty()) {
            const auto& e = unstable.back();
            raft_log.StableEntries(e.index(), e.term());
            CHECK(store_ptr->MayAppend(unstable));
        }

        const bool is_changed = raft_log.persisted() != w_persist;
        CHECK_EQ(raft_log.MaybePersist(stable_index, stable_term), is_changed);
        CHECK_EQ(raft_log.persisted(), w_persist);
    }

    {
        RaftLog raft_log(DefaultConfig(), std::make_unique<MemoryStorage>());
        raft_log.Restore(NewSnapshot(100, 1));
        CHECK_EQ(raft_log.unstable().offset(), 101);
        raft_log.Append({NewEntry(101, 1)});
        CHECK_EQ(raft_log.Term(101), 1);
        CHECK_FALSE(raft_log.MaybePersist(101, 1));
        raft_log.Append({NewEntry(102, 1)});
        CHECK_EQ(raft_log.Term(101), 1);
        CHECK_FALSE(raft_log.MaybePersist(102, 1));
    }
}

TEST_CASE("raft_log: unstable entries") {
    const std::vector previous_ents{NewEntry(1, 1), NewEntry(2, 2)};

    struct TestParam {
        uint64_t unstable = 0;
        std::vector<Entry> w_entries;
    };

    TestParam test;
    const std::vector<TestParam> tests{{3, {}}, {1, previous_ents}};
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [unstable, w_entries] = test;

    // append unstable entries to raft log
    auto store = std::make_unique<MemoryStorage>();
    if (unstable - 1 > 0) {
        store->Append(
            {previous_ents.begin(), previous_ents.begin() + unstable - 1}
        );
    }

    RaftLog raft_log(DefaultConfig(), std::move(store));
    if (unstable - 1 < previous_ents.size()) {
        raft_log.Append(
            {previous_ents.begin() + unstable - 1, previous_ents.end()}
        );
    }

    const auto ents = raft_log.unstable().entries();
    if (!ents.empty()) {
        const auto& e = ents.back();
        raft_log.StableEntries(e.index(), e.term());
    }
    CHECK_EQ(ents, w_entries);

    const auto w = previous_ents.back().index() + 1;
    const auto g = raft_log.unstable().offset();
    REQUIRE_EQ(w, g);
}

TEST_CASE("raft_log: has next entries and next entries") {
    const std::vector ents{
        NewEntry(4, 1),
        NewEntry(5, 1),
        NewEntry(6, 1),
        NewEntry(7, 1),
    };

    struct TestParam {
        uint64_t applied = 0;
        uint64_t persisted = 0;
        uint64_t committed = 0;
        std::optional<std::vector<Entry>> w_entries;
    };

    TestParam test;
    const std::vector<TestParam> tests{
        {0, 3, 3, std::nullopt},
        {0, 3, 4, std::nullopt},
        {0, 4, 6, {{ents.begin(), ents.begin() + 1}}},
        {0, 6, 4, {{ents.begin(), ents.begin() + 1}}},
        {0, 5, 5, {{ents.begin(), ents.begin() + 2}}},
        {0, 5, 7, {{ents.begin(), ents.begin() + 2}}},
        {0, 7, 5, {{ents.begin(), ents.begin() + 2}}},
        {3, 4, 3, std::nullopt},
        {3, 5, 5, {{ents.begin(), ents.begin() + 2}}},
        {3, 6, 7, {{ents.begin(), ents.begin() + 3}}},
        {3, 7, 6, {{ents.begin(), ents.begin() + 3}}},
        {4, 5, 5, {{ents.begin() + 1, ents.begin() + 2}}},
        {4, 5, 7, {{ents.begin() + 1, ents.begin() + 2}}},
        {4, 7, 5, {{ents.begin() + 1, ents.begin() + 2}}},
        {4, 7, 7, {{ents.begin() + 1, ents.begin() + 4}}},
        {5, 5, 5, std::nullopt},
        {5, 7, 7, {{ents.begin() + 2, ents.begin() + 4}}},
        {7, 7, 7, std::nullopt},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [applied, persisted, committed, w_entries] = test;

    auto store = std::make_unique<MemoryStorage>();
    auto* store_ptr = store.get();
    CHECK(store->ApplySnapshot(NewSnapshot(3, 1)));

    RaftLog raft_log(DefaultConfig(), std::move(store));
    raft_log.Append(ents);

    const auto unstable = raft_log.unstable().entries();
    if (!unstable.empty()) {
        const auto& e = unstable.back();
        raft_log.StableEntries(e.index(), e.term());
        store_ptr->Append(unstable);
    }

    raft_log.MaybePersist(persisted, 1);
    CHECK_EQ(persisted, raft_log.persisted());

    raft_log.MaybeCommit(committed, 1);
    CHECK_EQ(committed, raft_log.committed());

    raft_log.AppliedTo(applied);

    CHECK_EQ(w_entries.has_value(), raft_log.HasNextEntries());
    const auto next_entries = raft_log.NextEntries({});
    CHECK_EQ(w_entries, next_entries);
}

TEST_CASE("raft_log: has next entries and next entries, 2") {
    const std::vector ents{
        NewEntry(4, 1), NewEntry(5, 1), NewEntry(6, 1),  NewEntry(7, 1),
        NewEntry(8, 1), NewEntry(9, 1), NewEntry(10, 1),
    };

    struct TestParam {
        uint64_t applied = 0;
        uint64_t persisted = 0;
        uint64_t committed = 0;
        uint64_t limit = 0;
        std::optional<std::vector<Entry>> w_entries;
    };

    constexpr uint64_t UNLIMITED = std::numeric_limits<uint32_t>::max();
    TestParam test;
    const std::vector<TestParam> tests{
        {0, 3, 3, 0, std::nullopt},
        {0, 3, 4, 0, std::nullopt},
        {0, 3, 4, UNLIMITED, {{ents.begin(), ents.begin() + 1}}},
        {0, 4, 6, 0, {{ents.begin(), ents.begin() + 1}}},
        {0, 4, 6, 2, {{ents.begin(), ents.begin() + 3}}},
        {0, 4, 6, 6, {{ents.begin(), ents.begin() + 3}}},
        {0, 4, 10, 0, {{ents.begin(), ents.begin() + 1}}},
        {0, 4, 10, 2, {{ents.begin(), ents.begin() + 3}}},
        {0, 4, 10, 6, {ents}},
        {0, 4, 10, 7, {ents}},
        {0, 6, 4, 0, {{ents.begin(), ents.begin() + 1}}},
        {0, 6, 4, UNLIMITED, {{ents.begin(), ents.begin() + 1}}},
        {0, 5, 5, 0, {{ents.begin(), ents.begin() + 2}}},
        {3, 4, 3, UNLIMITED, std::nullopt},
        {3, 5, 5, UNLIMITED, {{ents.begin(), ents.begin() + 2}}},
        {3, 6, 7, UNLIMITED, {{ents.begin(), ents.begin() + 4}}},
        {3, 7, 6, UNLIMITED, {{ents.begin(), ents.begin() + 3}}},
        {4, 5, 5, UNLIMITED, {{ents.begin() + 1, ents.begin() + 2}}},
        {4, 5, 7, UNLIMITED, {{ents.begin() + 1, ents.begin() + 4}}},
        {4, 5, 9, UNLIMITED, {{ents.begin() + 1, ents.begin() + 6}}},
        {4, 5, 10, UNLIMITED, {{ents.begin() + 1, ents.end()}}},
        {4, 7, 5, UNLIMITED, {{ents.begin() + 1, ents.begin() + 2}}},
        {4, 7, 7, 0, {{ents.begin() + 1, ents.begin() + 4}}},
        {5, 5, 5, 0, std::nullopt},
        {5, 7, 7, UNLIMITED, {{ents.begin() + 2, ents.begin() + 4}}},
        {7, 7, 7, UNLIMITED, std::nullopt},
        // test applied can be bigger than `persisted + limit`(when limit is changed)
        {8, 6, 8, 0, std::nullopt},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [applied, persisted, committed, limit, w_entries] = test;

    auto store = std::make_unique<MemoryStorage>();
    auto* store_ptr = store.get();
    CHECK(store->ApplySnapshot(NewSnapshot(3, 1)));

    RaftLog raft_log(DefaultConfig(), std::move(store));
    raft_log.max_apply_unpersisted_log_limit() = limit;
    raft_log.Append(ents);

    const auto unstable = raft_log.unstable().entries();
    if (!unstable.empty()) {
        const auto& e = unstable.back();
        raft_log.StableEntries(e.index(), e.term());
        store_ptr->Append(unstable);
    }

    raft_log.MaybePersist(persisted, 1);
    CHECK_EQ(persisted, raft_log.persisted());

    raft_log.MaybeCommit(committed, 1);
    CHECK_EQ(committed, raft_log.committed());

    raft_log.AppliedTo(applied);

    CHECK_EQ(w_entries.has_value(), raft_log.HasNextEntries());
    const auto next_entries = raft_log.NextEntries({});
    CHECK_EQ(w_entries, next_entries);
}

TEST_CASE("raft_log: slice") {
    constexpr uint64_t offset = 100;
    constexpr uint64_t num = 100;
    constexpr uint64_t last = offset + num;
    constexpr uint64_t half = offset + num / 2;
    const Entry half_e = NewEntry(half, half);
    const auto half_e_size = half_e.ByteSizeLong();

    auto store = std::make_unique<MemoryStorage>();
    CHECK(store->ApplySnapshot(NewSnapshot(offset, 0)));
    for (uint64_t i = 1; i < num / 2; ++i) {
        store->Append({NewEntry(offset + i, offset + i)});
    }

    RaftLog raft_log(DefaultConfig(), std::move(store));
    for (uint64_t i = num / 2; i < num; ++i) {
        raft_log.Append({NewEntry(offset + i, offset + i)});
    }

    struct TestParam {
        uint64_t from = 0;
        uint64_t to = 0;
        uint64_t limit = 0;
        std::vector<Entry> w;
        bool w_panic = false;
        size_t test_index = 0;
    };

    constexpr auto NO_LIMIT = std::numeric_limits<uint64_t>::max();

    TestParam test;
    const std::vector<TestParam> tests{
        // test no limit
        {offset - 1, offset + 1, NO_LIMIT, {}, false},
        {offset, offset + 1, NO_LIMIT, {}, false},
        {
            half - 1,
            half + 1,
            NO_LIMIT,
            {NewEntry(half - 1, half - 1), NewEntry(half, half)},
            false,
        },
        {
            half,
            half + 1,
            NO_LIMIT,
            {NewEntry(half, half)},
            false,
        },
        {
            last - 1,
            last,
            NO_LIMIT,
            {NewEntry(last - 1, last - 1)},
            false,
        },
        {last, last + 1, NO_LIMIT, {}, true},
        // test limit
        {
            half - 1,
            half + 1,
            0,
            {NewEntry(half - 1, half - 1)},
            false,
        },
        {
            half - 1,
            half + 1,
            half_e_size + 1,
            {NewEntry(half - 1, half - 1)},
            false,
        },
        {
            half - 2,
            half + 1,
            half_e_size + 1,
            {NewEntry(half - 2, half - 2)},
            false,
        },
        {
            half - 1,
            half + 1,
            half_e_size * 2,
            {NewEntry(half - 1, half - 1), NewEntry(half, half)},
            false,
        },
        {
            half - 1,
            half + 2,
            half_e_size * 3,
            {
                NewEntry(half - 1, half - 1),
                NewEntry(half, half),
                NewEntry(half + 1, half + 1),
            },
            false,
        },
        {
            half,
            half + 2,
            half_e_size,
            {NewEntry(half, half)},
            false,
        },
        {
            half,
            half + 2,
            half_e_size * 2,
            {NewEntry(half, half), NewEntry(half + 1, half + 1)},
            false,
        },
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA_WITH_INDEX(test, tests);
    const auto& [from, to, limit, w, w_panic, test_index] = test;

    auto slice_result =
        raft_log.Slice(from, to, limit, GetEntriesContext::Empty(false), false);

    if (w_panic) {
        if (slice_result) {
            FAIL("expected error");
        }
        if (!slice_result.error().Is<FatalError>()) {
            FAIL("expected FatalError, but got: ", slice_result.error());
        }
        return;
    }

    // compacted
    if (from <= offset) {
        if (slice_result) {
            FAIL(
                "Expected Compacted error, but got OK. size: ",
                slice_result->size()
            );
        } else if (!slice_result.error().Is(StorageErrorCode::Compacted)) {
            FAIL("Expected Compacted error, but got: ", slice_result.error());
        }
        return;
    }

    REQUIRE_EQ(slice_result, w);
}

size_t ents_size(const std::vector<Entry>& ents) {
    return std::accumulate(
        ents.begin(), ents.end(), 0,
        [](const size_t previous, const Entry& entry) {
            return previous + entry.ByteSizeLong();
        }
    );
}

TEST_CASE("raft_log: scan") {
    auto offset = 47;
    auto num = 20;
    auto last = offset + num;
    auto half = offset + num / 2;
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
    store->Append(entries(offset + 1, half));
    RaftLog raft_log(DefaultConfig(), std::move(store));
    raft_log.Append(entries(half, last));

    size_t page_size = 0;
    const std::vector<size_t> page_sizes{0,   1,          10,
                                         100, entry_size, entry_size + 1};
    DOCTEST_VALUE_PARAMETERIZED_DATA(page_size, page_sizes);

    // Test that scan() returns the same entries as slice(), on all inputs.
    for (auto lo = offset + 1; lo <= last; ++lo) {
        for (auto hi = lo; hi <= last; ++hi) {
            std::vector<Entry> got;
            raft_log.Scan(
                lo, hi, page_size, GetEntriesContext::Empty(false),
                [&got, page_size](const std::vector<Entry>& ents) {
                    const bool ok =
                        ents.size() == 1 || ents_size(ents) < page_size;
                    CHECK(ok);
                    got.insert_range(got.end(), ents);
                    return true;
                }
            );
            auto want =
                raft_log.Slice(lo, hi, {}, GetEntriesContext::Empty(false));
            REQUIRE(want);
            CHECK_EQ(want, got);
        }
    }

    // Test that the callback early return.
    int iters = 0;
    REQUIRE(raft_log.Scan(
        offset + 1, half, 0, GetEntriesContext::Empty(false),
        [&iters](const std::vector<Entry>&) {
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
        offset + 1, offset + 11, entry_size * 2,
        GetEntriesContext::Empty(false),
        [entry_size](const std::vector<Entry>& ents) {
            CHECK_EQ(ents.size(), 2);
            CHECK_EQ(entry_size * 2, ents_size(ents));
            return true;
        }
    ));
}

TEST_SUITE_END();
