#include "raftpp/progress.h"

#include <doctest/doctest.h>

#include "raftpp/primitives.h"

using namespace raftpp;

TEST_SUITE_BEGIN("Progress");

ProgressDebug NewProgress(ProgressState state, uint64_t matched, uint64_t next_idx, uint64_t pending_snapshot) {
    ProgressDebug p(next_idx);
    p.state() = state;
    p.matched() = matched;
    p.pending_snapshot() = pending_snapshot;
    return p;
}

TEST_CASE("Progress is paused") {
    ProgressState state;
    bool paused;
    bool w;

    SUBCASE("") {
        state = ProgressState::Probe;
        paused = false;
        w = false;
    }
    SUBCASE("") {
        state = ProgressState::Probe;
        paused = true;
        w = true;
    }
    SUBCASE("") {
        state = ProgressState::Replicate;
        paused = false;
        w = false;
    }
    SUBCASE("") {
        state = ProgressState::Replicate;
        paused = true;
        w = false;
    }
    SUBCASE("") {
        state = ProgressState::Snapshot;
        paused = false;
        w = true;
    }
    SUBCASE("") {
        state = ProgressState::Snapshot;
        paused = true;
        w = true;
    }

    auto p = NewProgress(state, 0, 0, 0);
    p.paused() = paused;
    CHECK_EQ(w, p.IsPaused());
}

TEST_CASE("Progress resume") {
    ProgressDebug p(2);
    p.paused() = true;
    p.MaybeDecTo(1, 1, INVALID_INDEX);
    CHECK(!p.paused());
    p.paused() = true;
    p.MaybeUpdate(2);
    CHECK(!p.paused());
}

TEST_SUITE_END();
