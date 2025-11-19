#include "raftpp/progress.h"

#include <gtest/gtest.h>
#include <magic_enum/magic_enum.hpp>

#include "raftpp/primitives.h"

using namespace raftpp;

ProgressDebug NewProgress(ProgressState state, uint64_t matched, uint64_t next_idx, uint64_t pending_snapshot) {
    ProgressDebug p(next_idx);
    p.state() = state;
    p.matched() = matched;
    p.pending_snapshot() = pending_snapshot;
    return p;
}

TEST(ProgressTest, Resume) {
    ProgressDebug p(2);
    p.paused() = true;
    p.MaybeDecTo(1, 1, INVALID_INDEX);
    EXPECT_FALSE(p.paused());

    p.paused() = true;
    p.MaybeUpdate(2);
    EXPECT_FALSE(p.paused());
}

struct ProgressPausedTestParams {
    ProgressState state;
    bool paused;
    bool w;

    friend std::ostream& operator<<(std::ostream& os, const ProgressPausedTestParams& param) {
        return os << "initial_state=" << magic_enum::enum_name(param.state) << ", paused=" << param.paused;
    }
};

class ProgressPausedTest : public testing::TestWithParam<ProgressPausedTestParams> {};

TEST_P(ProgressPausedTest, Paused) {
    const auto [state, paused, w] = GetParam();
    auto p = NewProgress(state, 0, 0, 0);
    p.paused() = paused;
    EXPECT_EQ(w, p.IsPaused());
}

INSTANTIATE_TEST_SUITE_P(
    Probe, ProgressPausedTest,
    ::testing::ValuesIn<ProgressPausedTestParams>({
        {ProgressState::Probe, false, false},
        {ProgressState::Probe, true, true},
    })
);

INSTANTIATE_TEST_SUITE_P(
    Replicate, ProgressPausedTest,
    ::testing::ValuesIn<ProgressPausedTestParams>({
        {ProgressState::Replicate, false, false},
        {ProgressState::Replicate, true, false},
    })
);

INSTANTIATE_TEST_SUITE_P(
    Snapshot, ProgressPausedTest,
    ::testing::ValuesIn<ProgressPausedTestParams>({
        {ProgressState::Snapshot, false, true},
        {ProgressState::Snapshot, true, true},
    })
);
