/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The state of the test a runner is executing, as the assertions see it. The
// runner owns one RunState on its stack, publishes it before the runtime
// invokes the test, and the generated adapter binds the runtime's Context to
// it. The assertion macros reach it through current_state(), so a test body
// keeps the plain `void()` shape.
#ifndef KTEST_RUN_STATE_HPP
#define KTEST_RUN_STATE_HPP 1

#include <hsien/test.hpp>

namespace ktest::internal {

/// Longest skip reason the run state keeps. The runner prints it after the
/// test body has returned, so the reason is copied here rather than pointed
/// at; a longer one is truncated.
inline constexpr usize SKIP_REASON_ROOM = 96;

/// What the assertions and the runner share for one test. The fields are
/// written by the channel that owns each: the runner publishes the state, the
/// adapter binds |context|, and the assertion macros count |checks| and set
/// |skipped|. All of it happens on the thread running the test.
struct RunState {
    ::hsien::Context *context{nullptr};
    usize checks{};
    bool skipped{};
    char skip_reason[SKIP_REASON_ROOM]{};
};

/// The test being run on this thread, or nullptr when no test is running.
[[nodiscard]]
auto current_state() -> RunState *;

/// Whether the calling thread is the one the published test runs on. An
/// assertion from a worker thread must not touch the state, which lives on
/// the runner's stack, so the macros ask before they record anything.
[[nodiscard]]
auto on_test_thread() -> bool;

/// Reports a misuse of the framework, such as an assertion with no test
/// running. The reporter prints it at error level, so a run that hits one
/// fails even though no test recorded a failed check.
auto framework_error(char const *what) -> void;

/// Binds the runtime's Context to the published state; the generated adapter
/// does not call these directly.
auto bind_context(::hsien::Context &context) -> void;
auto unbind_context() -> void;

/// Marks the running test skipped. The reason is copied into the run state,
/// so a temporary string is fine; a longer one is truncated.
auto record_skip(char const *reason) -> void;

/// Publishes the calling thread as the owner of the stage the stage runner is
/// about to run — the thread the wrong-thread guard answers against — and
/// unbind_stage_thread() clears it after the stage. The kernel's
/// implementation tracks the runner thread itself; the host has one thread,
/// and its stand-in keeps the guard's answer fixed.
auto bind_stage_thread() -> void;
auto unbind_stage_thread() -> void;

/// Binds the Context for the lifetime of one test body. The generated
/// adapter holds one of these around the body it calls.
class BindScope final {
  public:
    explicit BindScope(::hsien::Context &context) {
        bind_context(context);
    }
    ~BindScope() {
        unbind_context();
    }
    BindScope(BindScope const &) = delete;
    auto operator=(BindScope const &) -> BindScope & = delete;
    BindScope(BindScope &&) = delete;
    auto operator=(BindScope &&) -> BindScope & = delete;
};

} // namespace ktest::internal

#endif // #ifndef KTEST_RUN_STATE_HPP
