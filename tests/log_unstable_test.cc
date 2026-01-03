#include <vector>

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include "raftpp/raftpp.pb.h"
#include "raftpp/unstable_log.h"
#include "raftpp/util.h"
#include "test_util.h"

using namespace raftpp;

namespace {

std::optional<Entry> NewEntry(const uint64_t index, const uint64_t term) {
    Entry ent;
    ent.set_term(term);
    ent.set_index(index);
    return ent;
}

}  // namespace

Snapshot NewSnapshot(const uint64_t index, const uint64_t term) {
    Snapshot snap;
    snap.mutable_metadata()->set_index(index);
    snap.mutable_metadata()->set_term(term);
    return snap;
}

struct LogUnstableTestParams {
    std::optional<Entry> ent;
    uint64_t offset;
    std::optional<Snapshot> snapshot;
    bool w_ok;
    uint64_t w_index;

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
