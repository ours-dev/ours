/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_DEFER_HPP
#define GKTL_DEFER_HPP

namespace gktl {
/// Runs an action when the scope ends, unless it was run early by invoke() or
/// dropped by cancel(). Whichever way it goes, the action runs at most once.
///
/// "At most once" is the whole contract, and it is what the old invoke() broke:
/// it ran the action without recording that it had, so the destructor ran it
/// again. Thread::suspend_or_kill() invokes its signal-setting action early,
/// under the target's PI lock, and the second run came from the destructor --
/// after the guard declared below it had released the lock and restored
/// interrupts -- as an unlocked read-modify-write of m_signals that could drop
/// a Kill another CPU had just set under the lock.
///
/// Two rules hold it to one run: invoke() marks the action spent before it
/// calls it, and a DeferredAction is not copyable.
template <typename Invocable_>
class DeferredAction final {
    using Self = DeferredAction;

  public:
    using Invocable = Invocable_;

    DeferredAction(Invocable invocable) : m_invoked(false), m_invocable(invocable) {}

    /// Not copyable: a copy would carry its own pending action, and the
    /// original and the copy would each run it when they went out of scope.
    /// defer() still returns by value -- it returns a prvalue, which C++17
    /// constructs straight into the caller's variable without copying.
    DeferredAction(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    ~DeferredAction() {
        invoke();
    }

    /// Runs the action now if it has not run and was not cancelled, and
    /// disarms the destructor.
    ///
    /// The flag is set before the call, not after: an action that reaches
    /// back into invoke() on its own DeferredAction then finds itself already
    /// spent, instead of recursing into a second run.
    ///
    /// There is deliberately no const overload. A const object cannot record
    /// that its action ran, so invoking it through one could only ever mean
    /// "run it now, and again at scope exit" -- the defect this replaces.
    auto invoke() -> void {
        if (m_invoked) {
            return;
        }
        m_invoked = true;
        m_invocable();
    }

    /// Drops the action without running it; the destructor then does nothing.
    auto cancel() -> void {
        m_invoked = true;
    }

  private:
    /// Whether the action is spent, run or cancelled. A scope object's one
    /// fact, so it has no word to share.
    bool m_invoked;
    Invocable m_invocable;
};

template <typename Invocable>
DeferredAction(Invocable invocable) -> DeferredAction<Invocable>;

template <typename Invocable>
auto defer(Invocable invocable) -> DeferredAction<Invocable> {
    return DeferredAction<Invocable>(invocable);
}
} // namespace gktl

#endif // #ifndef GKTL_DEFER_HPP
