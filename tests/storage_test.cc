#include "raftpp/storage.h"

#include <doctest/doctest.h>
#include <google/protobuf/util/message_differencer.h>
#include <spdlog/fmt/fmt.h>

#include "absl/strings/str_join.h"
#include "raftpp/memory_storage.h"
#include "spdlog/spdlog.h"
#include "test_util.h"

using namespace raftpp;

namespace {

Entry NewEntry(const uint64_t index, const uint64_t term) {
    Entry e;
    e.set_term(term);
    e.set_index(index);
    return e;
}

template <typename T>
size_t size_of(const T& m) {
    return m.ByteSizeLong();
}

}  // namespace

namespace raftpp {

bool operator==(const Entry& e1, const Entry& e2) {
    return google::protobuf::util::MessageDifferencer::Equals(e1, e2);
}

}  // namespace raftpp

template <>
struct fmt::formatter<std::vector<Entry>> : formatter<std::string_view> {
    static format_context::iterator format(const std::vector<Entry>& values, const format_context& ctx) {
        std::vector<std::string> s;
        s.reserve(values.size());
        for (const auto& v : values) {
            s.emplace_back(fmt::format("{{{}}}", v.ShortDebugString()));
        }
        return fmt::format_to(ctx.out(), "[\n{}\n]", absl::StrJoin(s, ",\n"));
    }
};

TEST_SUITE_BEGIN("Storage");

TEST_CASE("Term") {
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
    CHECK(term == wTerm);
}

TEST_CASE("Entries") {
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
        {4, 7, std::numeric_limits<uint64_t>::max(), std::vector{NewEntry(4, 4), NewEntry(5, 5), NewEntry(6, 6)}},
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
    CHECK(e == wEntries);
}

TEST_SUITE_END();
