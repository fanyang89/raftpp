#include "raftpp/storage.h"

#include <absl/strings/str_join.h>
#include <doctest/doctest.h>
#include <google/protobuf/util/message_differencer.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "harness/test_util.h"
#include "raftpp/memory_storage.h"
#include "test_util.h"

using namespace raftpp;

namespace {

template <typename T>
size_t size_of(const T& m) {
    return m.ByteSizeLong();
}

}  // namespace

template <>
struct fmt::formatter<std::vector<Entry>> : formatter<std::string_view> {
    static format_context::iterator format(
        const std::vector<Entry>& values, const format_context& ctx
    ) {
        std::vector<std::string> s;
        s.reserve(values.size());
        for (const auto& v : values) {
            s.emplace_back(fmt::format("{{{}}}", v.ShortDebugString()));
        }
        return fmt::format_to(ctx.out(), "[\n{}\n]", absl::StrJoin(s, ",\n"));
    }
};

TEST_SUITE_BEGIN("storage");

TEST_CASE("storage: term") {
    const std::vector entries{
        NewEntry(3, 3),
        NewEntry(4, 4),
        NewEntry(5, 5),
    };

    using TestParam = std::tuple<uint64_t, Result<uint64_t>>;
    TestParam test;
    std::vector<TestParam> tests{
        {2, RaftError(StorageErrorCode::Compacted)},
        {3, 3},
        {4, 4},
        {5, 5},
        {6, RaftError(StorageErrorCode::Unavailable)}
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [idx, wTerm] = test;

    MemoryStorage storage;
    storage.SetEntries(entries);
    const auto term = storage.Term(idx);
    CHECK_EQ(term, wTerm);
}

TEST_CASE("storage: entries") {
    const std::vector ents{
        NewEntry(3, 3),
        NewEntry(4, 4),
        NewEntry(5, 5),
        NewEntry(6, 6),
    };

    using TestParam = std::tuple<uint64_t, uint64_t, uint64_t, Result<std::vector<Entry>>>;
    TestParam test;
    std::vector<TestParam> tests{
        {2, 6, std::numeric_limits<uint64_t>::max(), RaftError(StorageErrorCode::Compacted)},
        {3, 4, std::numeric_limits<uint64_t>::max(), std::vector{NewEntry(3, 3)}},
        {4, 5, std::numeric_limits<uint64_t>::max(), std::vector{NewEntry(4, 4)}},
        {4, 6, std::numeric_limits<uint64_t>::max(), std::vector{NewEntry(4, 4), NewEntry(5, 5)}},
        {4, 7, std::numeric_limits<uint64_t>::max(),
         std::vector{NewEntry(4, 4), NewEntry(5, 5), NewEntry(6, 6)}},
        // even if maxsize is zero, the first entry should be returned
        {4, 7, 0, std::vector{NewEntry(4, 4)}},
        // limit to 2
        {4, 7, size_of(ents[1]) + size_of(ents[2]), std::vector{NewEntry(4, 4), NewEntry(5, 5)}},
        {
            4,
            7,
            size_of(ents[1]) + size_of(ents[2]) + size_of(ents[3]) / 2,
            std::vector{NewEntry(4, 4), NewEntry(5, 5)},
        },
        {
            4,
            7,
            size_of(ents[1]) + size_of(ents[2]) + size_of(ents[3]) - 1,
            std::vector{NewEntry(4, 4), NewEntry(5, 5)},
        },
        // all
        {
            4,
            7,
            size_of(ents[1]) + size_of(ents[2]) + size_of(ents[3]),
            std::vector{NewEntry(4, 4), NewEntry(5, 5), NewEntry(6, 6)},
        },
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [lo, hi, maxSize, wEntries] = test;

    MemoryStorage storage;
    storage.SetEntries(ents);
    const auto e = storage.Entries(lo, hi, maxSize, GetEntriesContext::Empty(false));
    CHECK_EQ(e, wEntries);
}

TEST_CASE("storage: last index") {
    const std::vector ents{
        NewEntry(3, 3),
        NewEntry(4, 4),
        NewEntry(5, 5),
    };
    MemoryStorage storage;
    storage.SetEntries(ents);

    auto result = storage.LastIndex();
    CHECK_EQ(5, result);

    storage.Append({NewEntry(6, 5)});
    result = storage.LastIndex();
    CHECK_EQ(6, result);
}

TEST_CASE("storage: first index") {
    const std::vector ents{
        NewEntry(3, 3),
        NewEntry(4, 4),
        NewEntry(5, 5),
    };

    MemoryStorage storage;
    storage.SetEntries(ents);
    CHECK_EQ(3, storage.FirstIndex());

    storage.Compact(4);
    CHECK_EQ(4, storage.FirstIndex());
}

TEST_CASE("storage: compact") {
    const std::vector ents{
        NewEntry(3, 3),
        NewEntry(4, 4),
        NewEntry(5, 5),
    };

    using TestParam = std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>;
    TestParam test;
    std::vector<TestParam> tests{
        {2, 3, 3, 3},
        {3, 3, 3, 3},
        {4, 4, 4, 2},
        {5, 5, 5, 1},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [idx, wIndex, wTerm, wLen] = test;

    MemoryStorage storage;
    storage.SetEntries(ents);
    storage.Compact(idx);

    uint64_t index = 0;
    if (const auto r = storage.FirstIndex(); r) {
        index = *r;
    } else {
        FAIL("FirstIndex()");
    }
    REQUIRE_EQ(wIndex, index);

    uint64_t term = 0;
    if (const auto r = storage.Entries(index, index + 1, 1, GetEntriesContext::Empty(false))) {
        if (!r->empty()) {
            term = r->front().term();
        }
    }
    REQUIRE_EQ(wTerm, term);

    uint64_t last = 0;
    if (const auto r = storage.LastIndex(); r) {
        last = *r;
    } else {
        FAIL("LastIndex()");
    }

    size_t len;
    if (const auto r = storage.Entries(index, last + 1, 100, GetEntriesContext::Empty(false)); r) {
        len = r->size();
    } else {
        FAIL("Entries()");
    }
    REQUIRE_EQ(wLen, len);
}

TEST_CASE("storage: create snapshot") {
    const std::vector ents{
        NewEntry(3, 3),
        NewEntry(4, 4),
        NewEntry(5, 5),
    };

    const std::vector<uint64_t> nodes{1, 2, 3};
    ConfState conf_state;
    conf_state.mutable_voters()->Add(nodes.begin(), nodes.end());

    RaftError unavailable(StorageErrorCode::SnapshotTemporarilyUnavailable);
    using TestParam = std::tuple<uint64_t, Result<Snapshot>, uint64_t>;
    TestParam test;
    std::vector<TestParam> tests{
        {4, NewSnapshot(4, 4, nodes), 0},
        {5, NewSnapshot(5, 5, nodes), 5},
        {5, NewSnapshot(6, 5, nodes), 6},
        {5, unavailable, 6},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto [idx, wResult, wIndex] = test;

    MemoryStorage storage;
    storage.SetEntries(ents);

    RaftState raft_state;
    raft_state.hard_state.set_commit(idx);
    raft_state.hard_state.set_term(idx);
    raft_state.conf_state.CopyFrom(conf_state);
    storage.SetRaftState(raft_state);

    if (!wResult.has_value()) {
        storage.TriggerSnapshotUnavailable();
    }

    const auto result = storage.GetSnapshot(wIndex, 0);
    CHECK_EQ(result, wResult);
}

TEST_CASE("storage: append") {
    const std::vector ents{
        NewEntry(3, 3),
        NewEntry(4, 4),
        NewEntry(5, 5),
    };

    struct TestParam {
        std::vector<Entry> entries;
        std::optional<std::vector<Entry>> wEntries;
    };

    TestParam test;
    const std::vector<TestParam> tests{
        {
            {NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 5)},
            std::make_optional(std::vector{NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 5)}),
        },
        {
            {NewEntry(3, 3), NewEntry(4, 6), NewEntry(5, 6)},
            std::make_optional(std::vector{NewEntry(3, 3), NewEntry(4, 6), NewEntry(5, 6)}),
        },
        {
            {NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 5), NewEntry(6, 5)},
            std::make_optional(
                std::vector{NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 5), NewEntry(6, 5)}
            ),
        },
        {{NewEntry(2, 3), NewEntry(3, 3), NewEntry(4, 5)}, std::nullopt},
        {
            {NewEntry(4, 5)},
            std::make_optional(std::vector{NewEntry(3, 3), NewEntry(4, 5)}),
        },
        {{NewEntry(6, 6)},
         std::make_optional(
             std::vector{NewEntry(3, 3), NewEntry(4, 4), NewEntry(5, 5), NewEntry(6, 6)}
         )}
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(test, tests);
    const auto& [entries, w_entries] = test;

    MemoryStorage storage;
    storage.SetEntries(ents);

    if (w_entries) {
        storage.Append(entries);
        CHECK_EQ(*w_entries, storage.AllEntries());
    } else {
        const auto r = storage.MayAppend(entries);
        CHECK(!r.has_value());
    }
}

TEST_CASE("storage: apply snapshot") {
    const std::vector<uint64_t> nodes{1, 2, 3};
    MemoryStorage storage;

    // Apply snapshot successfully
    auto snap = NewSnapshot(4, 4, nodes);
    if (auto r = storage.ApplySnapshot(snap); !r) {
        FAIL("ApplySnapshot()");
    }

    // Apply snapshot fails due to StorageError::SnapshotOutOfDate
    snap = NewSnapshot(3, 3, nodes);
    if (auto r = storage.ApplySnapshot(snap); r) {
        FAIL("ApplySnapshot()");
    }
}

TEST_SUITE_END();
