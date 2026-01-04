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

TEST_SUITE_BEGIN("LogUnstableTest");

TEST_CASE("Maybe first index") {
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

TEST_SUITE_END();
