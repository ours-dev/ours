/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The stage runner: what runs a suite hook or an environment stage the way a
// test runs, minus the test. One shared definition for the runner and the
// environment's default stages, header-inline so the kernel image and the
// host suite compile it against the same published state — which is what
// lets a hook's or a stage's assertions find the framing that published for
// them, on either side.
#ifndef KTEST_RUN_STAGE_HPP
#define KTEST_RUN_STAGE_HPP 1

#include <ktest/run_state.hpp>

#include <atomic>

namespace ktest::internal {

/// The test the stage runner is executing, published for the assertion
/// machinery. Header-inline because the runner and the environment walk share
/// one slot. Only the thread running the stage writes it, but a worker thread
/// that asserts by mistake reads it, so it is atomic: the read that is there
/// to diagnose the mistake must not race the store.
inline std::atomic<RunState *> CURRENT_STATE{nullptr};

/// Runs one hook — a suite's or an environment's SetUp or TearDown — the way
/// a test runs, minus the test: a published state and a context the hook's
/// assertions record into, no RUN or OK line, and nothing added to the
/// summary. True when nothing in the hook failed.
template <typename Step>
auto run_hook(char const *const name, Step &&step, ::hsien::Reporter &reporter) -> bool {
    RunState state;
    CURRENT_STATE.store(&state, std::memory_order_relaxed);
    bind_stage_thread();
    auto context = ::hsien::Context{name, reporter};
    BindScope const scope(context);
    step(context);
    CURRENT_STATE.store(nullptr, std::memory_order_relaxed);
    unbind_stage_thread();
    return context.failure_count() == 0;
}

} // namespace ktest::internal

#endif // #ifndef KTEST_RUN_STAGE_HPP
