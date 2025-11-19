#include "raftpp/log_unstable.h"

#include <vector>

#include <gtest/gtest.h>
#include <spdlog/fmt/fmt.h>

#include "raftpp/raftpp.pb.h"
#include "raftpp/util.h"

using namespace raftpp;

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

class LogUnstableTest : public testing::TestWithParam<LogUnstableTestParams> {};

TEST_P(LogUnstableTest, MaybeFirstIndex) {
    const auto& [ent, offset, snapshot, w_ok, w_index] = GetParam();

    size_t entries_size = 0;
    std::vector<Entry> entries;
    if (ent) {
        entries.emplace_back(*ent);
        entries_size += EntryApproximateSize(*ent);
    }

    const Unstable u(entries, entries_size, offset, snapshot);
    if (const auto index = u.MaybeFirstIndex(); index) {
        EXPECT_EQ(w_index, index);
    } else {
        EXPECT_FALSE(w_ok);
    }
}

INSTANTIATE_TEST_SUITE_P(
    NoSnapshot, LogUnstableTest,
    ::testing::ValuesIn<LogUnstableTestParams>({
        {NewEntry(5, 1), 5, {}, false, 0},
        {{}, 0, {}, false, 0},
    })
);

INSTANTIATE_TEST_SUITE_P(
    HasSnapshot, LogUnstableTest,
    ::testing::ValuesIn<LogUnstableTestParams>({
        {NewEntry(5, 1), 5, NewSnapshot(4, 1), true, 5},
        {{}, 5, NewSnapshot(4, 1), true, 5},
    })
);
