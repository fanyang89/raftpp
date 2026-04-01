#include "raftpp/core/inflights.h"

#include <doctest/doctest.h>

#include "test_util.h"

using namespace raftpp;

class InflightsDebug : public Inflights {
  public:
    using Inflights::Inflights;

    [[nodiscard]] size_t& start();
    [[nodiscard]] size_t count() const;
    [[nodiscard]] std::vector<uint64_t>& buffer();
    [[nodiscard]] size_t capacity() const;
    [[nodiscard]] std::optional<size_t> incoming_capacity() const;
};

size_t& InflightsDebug::start() {
    return start_;
}

size_t InflightsDebug::count() const {
    return count_;
}

std::vector<uint64_t>& InflightsDebug::buffer() {
    return buffer_;
}

size_t InflightsDebug::capacity() const {
    return capacity_;
}

std::optional<size_t> InflightsDebug::incoming_capacity() const {
    return incoming_capacity_;
}

TEST_SUITE_BEGIN("inflights");

TEST_CASE("inflights: Add") {
    InflightsDebug inflight(10);

    for (uint64_t i = 0; i < 5; ++i) {
        inflight.Add(i);
    }
    CHECK_EQ(inflight.start(), 0);
    CHECK_EQ(inflight.count(), 5);
    CHECK_EQ(inflight.buffer(), std::vector<uint64_t>{0, 1, 2, 3, 4});
    CHECK_EQ(inflight.capacity(), 10);
    CHECK_EQ(inflight.incoming_capacity(), std::nullopt);

    for (uint64_t i = 5; i < 10; ++i) {
        inflight.Add(i);
    }
    CHECK_EQ(inflight.start(), 0);
    CHECK_EQ(inflight.count(), 10);
    CHECK_EQ(inflight.buffer(), std::vector<uint64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK_EQ(inflight.capacity(), 10);
    CHECK_EQ(inflight.incoming_capacity(), std::nullopt);

    InflightsDebug inflight2(10);
    inflight2.start() = 5;
    auto filler = std::vector<uint64_t>{0, 0, 0, 0, 0};
    inflight2.buffer().insert(inflight2.buffer().end(), filler.begin(), filler.end());

    for (uint64_t i = 0; i < 5; ++i) {
        inflight2.Add(i);
    }
    CHECK_EQ(inflight2.start(), 5);
    CHECK_EQ(inflight2.count(), 5);
    CHECK_EQ(inflight2.buffer(), std::vector<uint64_t>{0, 0, 0, 0, 0, 0, 1, 2, 3, 4});
    CHECK_EQ(inflight2.capacity(), 10);
    CHECK_EQ(inflight2.incoming_capacity(), std::nullopt);

    for (uint64_t i = 5; i < 10; ++i) {
        inflight2.Add(i);
    }
    CHECK_EQ(inflight2.start(), 5);
    CHECK_EQ(inflight2.count(), 10);
    CHECK_EQ(inflight2.buffer(), std::vector<uint64_t>{5, 6, 7, 8, 9, 0, 1, 2, 3, 4});
    CHECK_EQ(inflight2.capacity(), 10);
    CHECK_EQ(inflight2.incoming_capacity(), std::nullopt);
}

TEST_CASE("inflights: FreeTo") {
    InflightsDebug inflight(10);
    for (uint64_t i = 0; i < 10; ++i) {
        inflight.Add(i);
    }

    inflight.FreeTo(4);
    CHECK_EQ(inflight.start(), 5);
    CHECK_EQ(inflight.count(), 5);
    CHECK_EQ(inflight.buffer(), std::vector<uint64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK_EQ(inflight.capacity(), 10);
    CHECK_EQ(inflight.incoming_capacity(), std::nullopt);

    inflight.FreeTo(8);
    CHECK_EQ(inflight.start(), 9);
    CHECK_EQ(inflight.count(), 1);
    CHECK_EQ(inflight.buffer(), std::vector<uint64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK_EQ(inflight.capacity(), 10);
    CHECK_EQ(inflight.incoming_capacity(), std::nullopt);

    for (uint64_t i = 10; i < 15; ++i) {
        inflight.Add(i);
    }

    inflight.FreeTo(12);
    CHECK_EQ(inflight.start(), 3);
    CHECK_EQ(inflight.count(), 2);
    CHECK_EQ(inflight.buffer(), std::vector<uint64_t>{10, 11, 12, 13, 14, 5, 6, 7, 8, 9});
    CHECK_EQ(inflight.capacity(), 10);
    CHECK_EQ(inflight.incoming_capacity(), std::nullopt);

    inflight.FreeTo(14);
    CHECK_EQ(inflight.start(), 5);
    CHECK_EQ(inflight.count(), 0);
    CHECK_EQ(inflight.buffer(), std::vector<uint64_t>{10, 11, 12, 13, 14, 5, 6, 7, 8, 9});
    CHECK_EQ(inflight.capacity(), 10);
    CHECK_EQ(inflight.incoming_capacity(), std::nullopt);
}

TEST_CASE("inflights: FreeFirstOne") {
    InflightsDebug inflight(10);
    for (uint64_t i = 0; i < 10; ++i) {
        inflight.Add(i);
    }

    inflight.FreeFirstOne();
    CHECK_EQ(inflight.start(), 1);
    CHECK_EQ(inflight.count(), 9);
    CHECK_EQ(inflight.buffer(), std::vector<uint64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK_EQ(inflight.capacity(), 10);
    CHECK_EQ(inflight.incoming_capacity(), std::nullopt);
}

TEST_CASE("inflights: SetCapacity") {
    int64_t start = 0;
    std::vector<uint64_t> starts{16, 112, 120};
    DOCTEST_VALUE_PARAMETERIZED_DATA(start, starts);

    InflightsDebug inflight(128);
    for (uint64_t i = 0; i < start; ++i) {
        inflight.Add(i);
    }
    inflight.FreeTo(start - 1);
    for (uint64_t i = 0; i < 16; ++i) {
        inflight.Add(i);
    }
    CHECK_EQ(inflight.count(), 16);
    CHECK_EQ(inflight.start(), start);

    inflight.SetCapacity(1024);
    CHECK_EQ(inflight.capacity(), 1024);
    CHECK_EQ(inflight.incoming_capacity(), std::nullopt);
    REQUIRE_EQ(inflight.buffer().capacity(), 1024);
    if (start != 120) {
        CHECK_NE(inflight.start(), 0);
    } else {
        CHECK_EQ(inflight.start(), 0);
    }
}

TEST_SUITE_END();
