#include "raftpp/log_unstable.h"

#include <vector>
#include <doctest/doctest.h>

#include "raftpp/raftpp.pb.h"
#include "raftpp/util.h"

using namespace raftpp;

TEST_SUITE_BEGIN("log unstable");

std::optional<Entry> NewEntry(const uint64_t index, const uint64_t term) {
    Entry ent;
    ent.set_term(term);
    ent.set_index(index);
    return ent;
}

Snapshot NewSnapshot(const uint64_t index, const uint64_t term) {
    Snapshot snap;
    snap.mutable_metadata()->set_index(index);
    snap.mutable_metadata()->set_term(term);
    return snap;
}

TEST_CASE("Maybe first index") {
    std::tuple<std::optional<Entry>, uint64_t, std::optional<Snapshot>, bool, uint64_t> test;
    // no snapshot
    SUBCASE("") { test = {NewEntry(5, 1), 5, {}, false, 0}; }
    SUBCASE("") { test = {{}, 0, {}, false, 0}; }
    // has snapshot
    SUBCASE("") { test = {NewEntry(5, 1), 5, NewSnapshot(4, 1), true, 5}; }
    SUBCASE("") { test = {{}, 5, NewSnapshot(4, 1), true, 5}; }

    std::optional<Entry> e;
    uint64_t offset;
    std::optional<Snapshot> snapshot;
    bool wOk;
    uint64_t wIndex;
    std::tie(e, offset, snapshot, wOk, wIndex) = test;

    size_t entries_size = 0;
    std::vector<Entry> entries;
    if (e) {
        entries.emplace_back(*e);
        entries_size += EntryApproximateSize(*e);
    }

    Unstable u(entries, entries_size, offset, snapshot);
    if (auto index = u.MaybeFirstIndex(); index) {
        CHECK_EQ(wIndex, index);
    } else {
        CHECK(!wOk);
    }
}

TEST_SUITE_END();
