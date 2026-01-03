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

    using TestParam = std::tuple<std::vector<Entry>, uint64_t>;
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
    const auto [ents, wConflict] = test;

    Config config;
    auto store = std::make_unique<MemoryStorage>();
    RaftLog raft_log(config, std::move(store));
    raft_log.Append(previous_entries);

    const auto r = raft_log.FindConflict(ents);
    CHECK(wConflict == r);
}

TEST_SUITE_END();
