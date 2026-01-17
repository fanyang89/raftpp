#include "raftpp/progress.h"

#include <doctest/doctest.h>

#include "raftpp/primitives.h"
#include "test_util.h"

using namespace raftpp;

ProgressDebug NewProgress(
    const ProgressState state, const uint64_t matched, const uint64_t next_idx,
    const uint64_t pending_snapshot
) {
    ProgressDebug p(next_idx);
    p.state() = state;
    p.matched() = matched;
    p.pending_snapshot() = pending_snapshot;
    return p;
}

struct ProgressPausedTestParams {
    ProgressState state;
    bool paused;
    bool w;

    friend std::ostream& operator<<(std::ostream& os, const ProgressPausedTestParams& param) {
        return os << "initial_state=" << format_as(param.state) << ", paused=" << param.paused;
    }
};

TEST_SUITE_BEGIN("progress");

TEST_CASE("progress: resume") {
    ProgressDebug p(2);
    p.paused() = true;
    std::ignore = p.MaybeDecTo(1, 1, INVALID_INDEX);
    CHECK_FALSE(p.paused());

    p.paused() = true;
    std::ignore = p.MaybeUpdate(2);
    CHECK_FALSE(p.paused());
}

TEST_CASE("progress: paused") {
    ProgressPausedTestParams params{};
    const std::list<ProgressPausedTestParams> tests{
        // probe
        {ProgressState::Probe, false, false},
        {ProgressState::Probe, true, true},
        // Replicate
        {ProgressState::Replicate, false, false},
        {ProgressState::Replicate, true, false},
        // Snapshot
        {ProgressState::Snapshot, false, true},
        {ProgressState::Snapshot, true, true},
    };
    DOCTEST_VALUE_PARAMETERIZED_DATA(params, tests);
    const auto [state, paused, w] = params;
    auto p = NewProgress(state, 0, 0, 0);
    p.paused() = paused;
    CHECK_EQ(w, p.IsPaused());
}

TEST_CASE("progress: become probe") {
    struct TestCase {
        ProgressState initial_state;
        uint64_t matched;
        uint64_t next_idx;
        uint64_t pending_snapshot;
        uint64_t expected_next_idx;
        std::string description;
    };

    const std::vector<TestCase> tests{
        {ProgressState::Replicate, 1, 5, 0, 2, "from replicate"},
        {ProgressState::Snapshot, 1, 5, 10, 11, "from snapshot (finished)"},
        {ProgressState::Snapshot, 1, 5, 0, 2, "from snapshot (failed)"},
    };

    for (const auto& test : tests) {
        SUBCASE(test.description.c_str()) {
            ProgressDebug p(test.next_idx, 256);
            p.state() = test.initial_state;
            p.matched() = test.matched;
            p.pending_snapshot() = test.pending_snapshot;

            p.BecomeProbe();

            CHECK_EQ(p.state(), ProgressState::Probe);
            CHECK_EQ(p.matched(), test.matched);
            CHECK_EQ(p.next_idx(), test.expected_next_idx);
        }
    }
}

TEST_CASE("progress: become replicate") {
    ProgressDebug p(5, 256);
    p.state() = ProgressState::Probe;
    p.matched() = 1;

    p.BecomeReplicate();

    CHECK_EQ(p.state(), ProgressState::Replicate);
    CHECK_EQ(p.matched(), 1);
    CHECK_EQ(p.matched() + 1, p.next_idx());
}

TEST_CASE("progress: become snapshot") {
    ProgressDebug p(5, 256);
    p.state() = ProgressState::Probe;
    p.matched() = 1;

    p.BecomeSnapshot(10);

    CHECK_EQ(p.state(), ProgressState::Snapshot);
    CHECK_EQ(p.matched(), 1);
    CHECK_EQ(p.pending_snapshot(), 10);
}

TEST_CASE("progress: update") {
    const uint64_t prev_matched = 3;
    const uint64_t prev_next = 5;

    struct TestCase {
        uint64_t update;
        uint64_t expected_matched;
        uint64_t expected_next;
        bool expected_ok;
        std::string description;
    };

    const std::vector<TestCase> tests{
        {prev_matched - 1, prev_matched, prev_next, false, "update less than prev_matched"},
        {prev_matched, prev_matched, prev_next, false, "update equals prev_matched"},
        {prev_matched + 1, prev_matched + 1, prev_next, true, "update greater than prev_matched"},
        {prev_matched + 2, prev_matched + 2, prev_next + 1, true,
         "update far greater than prev_matched"},
    };

    for (const auto& test : tests) {
        SUBCASE(test.description.c_str()) {
            ProgressDebug p(prev_next, 256);
            p.matched() = prev_matched;

            const bool ok = p.MaybeUpdate(test.update);

            CHECK_EQ(ok, test.expected_ok);
            CHECK_EQ(p.matched(), test.expected_matched);
            CHECK_EQ(p.next_idx(), test.expected_next);
        }
    }
}

TEST_CASE("progress: maybe decr") {
    struct TestCase {
        ProgressState state;
        uint64_t matched;
        uint64_t next_idx;
        uint64_t rejected;
        uint64_t last;
        bool expected_changed;
        uint64_t expected_next;
        std::string description;
    };

    const std::vector<TestCase> tests{
        // state replicate and rejected is not greater than match
        {ProgressState::Replicate, 5, 10, 5, 5, false, 10, "replicate: rejected <= matched"},
        // state replicate and rejected is not greater than match
        {ProgressState::Replicate, 5, 10, 4, 4, false, 10, "replicate: rejected < matched"},
        // state replicate and rejected is greater than match
        // directly decrease to match+1
        {ProgressState::Replicate, 5, 10, 9, 9, true, 6, "replicate: rejected > matched"},
        // next-1 != rejected is always false
        {ProgressState::Probe, 0, 0, 0, 0, false, 0, "probe: next-1 != rejected"},
        // next-1 != rejected is always false
        {ProgressState::Probe, 0, 10, 5, 5, false, 10, "probe: next-1 != rejected"},
        // next>1 = decremented by 1
        {ProgressState::Probe, 0, 10, 9, 9, true, 9, "probe: decremented by 1"},
        // next>1 = decremented by 1
        {ProgressState::Probe, 0, 2, 1, 1, true, 1, "probe: decremented by 1 (next=2)"},
        // next<=1 = reset to 1
        {ProgressState::Probe, 0, 1, 0, 0, true, 1, "probe: next<=1, reset to 1"},
        // decrease to min(rejected, last+1)
        {ProgressState::Probe, 0, 10, 9, 2, true, 3, "probe: decrease to min(rejected, last+1)"},
        // rejected < 1, reset to 1
        {ProgressState::Probe, 0, 10, 9, 0, true, 1, "probe: rejected < 1, reset to 1"},
    };

    for (const auto& test : tests) {
        SUBCASE(test.description.c_str()) {
            ProgressDebug p(test.next_idx, 256);
            p.state() = test.state;
            p.matched() = test.matched;

            const bool changed = p.MaybeDecTo(test.rejected, test.last, INVALID_INDEX);

            CHECK_EQ(changed, test.expected_changed);
            CHECK_EQ(p.matched(), test.matched);
            CHECK_EQ(p.next_idx(), test.expected_next);
        }
    }
}

TEST_SUITE_END();
