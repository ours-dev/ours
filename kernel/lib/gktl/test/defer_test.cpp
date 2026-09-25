/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// gktl::DeferredAction promises one thing: its action runs at most once,
/// whichever of scope exit, invoke() and cancel() comes first. invoke() used
/// to run the action without recording that it had, so the destructor ran it
/// a second time -- Thread::suspend_or_kill() set a thread's signal once under
/// its PI lock and once more, unlocked, after the guard had released it. Each
/// case below counts runs, so "ran twice" is a failed assertion rather than a
/// race nobody sees.

#include <gktl/defer.hpp>

#include <type_traits>

#include <cassert>

namespace {

/// Counts how many times the deferred action ran. Captured by reference, so
/// the count outlives the DeferredAction and can be read after its scope.
struct RunCount {
    auto bump() -> void {
        runs += 1;
    }

    unsigned runs{0};
};

// A copy would carry a second pending action.
static_assert(!std::is_copy_constructible_v<decltype(gktl::defer([] {}))>);
static_assert(!std::is_copy_assignable_v<decltype(gktl::defer([] {}))>);

auto test_runs_at_scope_exit() -> void {
    RunCount count;
    {
        auto const action = gktl::defer([&count] { count.bump(); });
        assert(count.runs == 0);
    }
    assert(count.runs == 1);
}

auto test_invoke_runs_once() -> void {
    RunCount count;
    {
        auto action = gktl::defer([&count] { count.bump(); });
        action.invoke();
        assert(count.runs == 1);

        // A second invoke() is a no-op, not a second run.
        action.invoke();
        assert(count.runs == 1);
    }
    // The case suspend_or_kill() hit: invoke() early, then scope exit.
    assert(count.runs == 1);
}

auto test_cancel_suppresses() -> void {
    RunCount count;
    {
        auto action = gktl::defer([&count] { count.bump(); });
        action.cancel();

        // Cancelled means spent: invoke() after cancel() does not run it.
        action.invoke();
    }
    assert(count.runs == 0);
}

/// An action that calls invoke() on its own DeferredAction. It has to name
/// its own type, which a lambda cannot, hence the hand-written functor.
struct Reenter {
    auto operator()() const -> void {
        count->bump();
        (*self)->invoke();
    }

    RunCount *count;
    gktl::DeferredAction<Reenter> **self;
};

auto test_reentrant_invoke_is_spent() -> void {
    // The flag is set before the action is called, so the action's own
    // invoke() finds it already spent. Setting it after the call instead
    // would recurse until the stack ran out.
    RunCount count;
    gktl::DeferredAction<Reenter> *self = nullptr;
    {
        gktl::DeferredAction<Reenter> action{Reenter{&count, &self}};
        self = &action;
        action.invoke();
        assert(count.runs == 1);
    }
    assert(count.runs == 1);
}

} // namespace

auto main() -> int {
    test_runs_at_scope_exit();
    test_invoke_runs_once();
    test_cancel_suppresses();
    test_reentrant_invoke_is_spent();
    return 0;
}
