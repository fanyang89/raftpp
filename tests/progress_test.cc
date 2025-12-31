#include "raftpp/progress.h"

#include <doctest/doctest.h>
#include <magic_enum/magic_enum.hpp>

#include "raftpp/primitives.h"
#include "test_util.h"

using namespace raftpp;

ProgressDebug NewProgress(ProgressState state, uint64_t matched, uint64_t next_idx, uint64_t pending_snapshot) {
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
        return os << "initial_state=" << magic_enum::enum_name(param.state) << ", paused=" << param.paused;
    }
};

TEST_SUITE_BEGIN("ProgressTest");

TEST_CASE("Resume") {
    ProgressDebug p(2);
    p.paused() = true;
    p.MaybeDecTo(1, 1, INVALID_INDEX);
    CHECK_FALSE(p.paused());

    p.paused() = true;
    p.MaybeUpdate(2);
    CHECK_FALSE(p.paused());
}

TEST_CASE("Paused") {
    ProgressPausedTestParams params;
    std::list<ProgressPausedTestParams> tests{
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

TEST_SUITE_END();
