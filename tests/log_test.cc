#include <doctest/doctest.h>

#include "raftpp/memory_storage.h"
#include "raftpp/raft_log.h"
#include "raftpp/raftpp.pb.h"
#include "test_util.h"

using namespace raftpp;

TEST_SUITE_BEGIN("Log");

TEST_CASE("Find conflict") {
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
    std::vector<TestParam> tests{
        // no conflict, empty ent
        {{}, 0},
        // no conflict
        {{NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3)}, 0},
        {{NewEntry(2, 2), NewEntry(3, 3)}, 0},
        {{NewEntry(3, 3)}, 0},
        // no conflict, but has new entries
        {{NewEntry(1, 1), NewEntry(2, 2), NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 4)}, 4},
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

TEST_CASE("is up-to-date") {
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

TEST_SUITE_END();
