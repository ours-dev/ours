/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TIMER_HPP
#define OURS_TIME_TIMER_HPP 1

#include <ours/mm/cpu_local.hpp>
#include <ours/platform/timer.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/mod.hpp>
#include <ours/time/types.hpp>

#include <atomic>

#include <gktl/canary.hpp>
#include <ustl/collections/intrusive/any_hook.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/function/function.hpp>

namespace ours::time {
/// How much later than its deadline a timer may run.
///
/// Slack exists so that timers due at nearly the same moment can be made to
/// fire together, one interrupt instead of several. The coalescing itself is
/// not implemented here yet (TimerQueue::enqueue_locked() inserts by deadline
/// and nothing merges), so today the only effect is the one below: a timer
/// with slack is ordered and expired by its latest permissible instant, i.e.
/// it always takes the whole slack. That is the "late" policy; when
/// coalescing arrives this class is where an early/late/centre mode would go.
class TimeSlack {
    typedef TimeSlack Self;

  public:
    FORCE_INLINE
    constexpr TimeSlack() = default;

    /// Explicit: a bare duration is far more often a deadline than a slack,
    /// and the two used to be interconvertible.
    template <typename DurationT>
    FORCE_INLINE
    constexpr explicit TimeSlack(DurationT duration)
        : m_slack(duration_cast<Duration>(duration)) {}

    [[nodiscard]]
    FORCE_INLINE
    constexpr auto duration() const -> Duration {
        return m_slack;
    }

  private:
    // Value-initialised: a Deadline built without a slack must mean "no
    // slack", and a default-constructed chrono duration is indeterminate.
    Duration m_slack{};
};

/// When a timer is due, as an absolute instant plus the slack allowed on it.
///
/// Built only through the named factories. It used to be constructible
/// straight from a Duration, which made `Deadline(Milliseconds(10))` compile
/// and mean "10 ms after the epoch" -- a deadline decades in the past --
/// rather than the "10 ms from now" it reads as. And the default constructor
/// gave the epoch, so `Deadline()` was a deadline that had already passed,
/// while every caller that wrote it meant "no deadline". Both are now spelled
/// out: at() takes an instant, infinite() is the default.
///
/// There is no after(Duration) yet. Every caller so far has just read the
/// clock for its own purposes and can pass the instant; when one turns up
/// that has not, its home is here, next to at().
class Deadline {
    using Self = Deadline;

  public:
    /// No deadline at all -- the common case for a wait that is bounded by
    /// something other than time.
    FORCE_INLINE
    constexpr Deadline()
        : m_time_point(INFINITE_TIME), m_time_slack() {}

    /// Due at |when|, and no later than |when| plus the slack.
    FORCE_INLINE
    constexpr static auto at(TimePoint<> when, TimeSlack slack = {}) -> Self {
        return Self(when, slack);
    }

    FORCE_INLINE
    constexpr static auto infinite() -> Self {
        return Self();
    }

    [[nodiscard]]
    FORCE_INLINE
    constexpr auto when() const -> TimePoint<> {
        return m_time_point;
    }

    [[nodiscard]]
    FORCE_INLINE
    constexpr auto earliest() const -> TimePoint<> {
        return m_time_point;
    }

    [[nodiscard]]
    FORCE_INLINE
    constexpr auto latest() const -> TimePoint<> {
        return m_time_point + m_time_slack.duration();
    }

    [[nodiscard]]
    FORCE_INLINE
    constexpr auto slack() const -> TimeSlack {
        return m_time_slack;
    }

    FORCE_INLINE
    constexpr friend auto operator==(Self const &x, Self const &y) -> bool {
        return x.m_time_point == y.m_time_point;
    }

    FORCE_INLINE
    constexpr friend auto operator!=(Self const &x, Self const &y) -> bool {
        return x.m_time_point != y.m_time_point;
    }

  private:
    FORCE_INLINE
    constexpr Deadline(TimePoint<> when, TimeSlack slack)
        : m_time_point(when), m_time_slack(slack) {}

    TimePoint<> m_time_point;
    TimeSlack m_time_slack;
};

enum class ClockId {
    Monotonic,
    Boottime,
    MaxNumClocks,
};

/// A one-shot callback at a deadline, on the CPU that armed it.
///
/// The object is meant to live on the stack of the thread that waits on it
/// (Thread::Current::sleep_etc(), WaitQueue::park_thread_postamble()), which
/// fixes the one rule everything below serves: **when cancel() returns, the
/// handler is not running and will not run**, so the Timer may be destroyed.
/// A cancel that merely set a flag -- what this class did at first -- left
/// the node in the queue after the stack frame was gone.
///
/// Three pieces of state, and the protocol around them, keep that promise:
///
///  * m_queue: the TimerQueue the timer was last put on. It is the lock to
///    take for any change to the timer, found by load, lock, re-check
///    (TimerQueue::with_queue_locked()).
///
///  * m_active_cpu: the CPU whose TimerQueue is running the handler right
///    now, or INVALID_CPU_NUM. cancel() spins on it after unqueueing, which
///    is what makes "not running" true and not just likely.
///
///  * m_cancelled: set by cancel() before anything else. A handler that
///    needs a lock the cancelling thread holds cannot just take it -- the
///    canceller is spinning on m_active_cpu, so both would wait forever --
///    and trylock_or_cancel() is the way out: try the lock, and give up as
///    soon as this flag says the timer is being cancelled.
///
///  * m_parked: the hook is in the queue's list of timers to restart, not in
///    its set. A pass puts a restarting timer there instead of back in the set
///    so that the device is armed once, at the end of the pass, and not at
///    every restart. is_linked() cannot tell the two containers apart and
///    cancel() must not erase a list node from the set, so this bit says which
///    one; it is read and written only under the lock of m_queue, and cancel()
///    takes a parked timer out of the list itself, since the pass that parked
///    it may not be done for a while.
class Timer {
    using Self = Timer;

  public:
    enum class HandlerResult {
        /// The handler is finished; the timer stays disarmed.
        Handled,

        /// Re-arm at the (forwarded) deadline. The handler must have moved
        /// the deadline with forward() first, or it expires again at once.
        Restart,
    };
    /// Runs in interrupt context, so it must not allocate: a Function keeps
    /// its target inline and refuses at compile time one larger than 24
    /// bytes, where the std::function this used to be moved such a target to
    /// the heap without a word.
    using Handler = ustl::function::Function<auto(Timer *)->HandlerResult>;

    constexpr Timer(ClockId clkid = ClockId::Monotonic)
        : m_canary(this)
        , m_clockid(clkid)
        , m_queue(nullptr)
        , m_deadline()
        , m_on_expired()
        , m_active_cpu(INVALID_CPU_NUM)
        , m_cancelled(false) {}

    ~Timer() {
        // Destroyed while queued or while its handler runs: either is a
        // use-after-free in waiting, and means a cancel() was skipped.
        DEBUG_ASSERT(!is_linked());
        DEBUG_ASSERT(m_active_cpu.load(std::memory_order_relaxed) == INVALID_CPU_NUM);
    }

    template <typename F, typename... Args>
    FORCE_INLINE
    auto activate(Deadline deadline, F &&f, Args &&...args) -> void {
        Handler on_expired =
            ustl::function::bind_front(ustl::forward<F>(f), ustl::forward<Args>(args)...);
        return activate(deadline, ustl::move(on_expired));
    }

    /// Arms the timer on the current CPU's TimerQueue. |on_expired| runs in
    /// interrupt context on that CPU, with the queue lock *not* held, so it
    /// may activate() or cancel() this or any other timer.
    ///
    /// Activating a queued timer is a bug, and fails an assertion.
    /// Activating from inside the timer's own handler is allowed and is how
    /// a handler re-arms itself; it is a no-op if a cancel() has raced in.
    auto activate(Deadline deadline, Handler on_expired) -> void;

    /// Advances the deadline by whole multiples of |interval| until it is
    /// after |now|, and returns how many intervals were skipped. For periodic
    /// timers that must not drift and must not try to catch up on every
    /// missed period one by one. Only for a timer that is not queued: the
    /// deadline is the queue's sort key.
    auto forward(TimePoint<> now, Duration interval) -> u64;

    auto forward(Duration interval) -> u64;

    /// Disarms the timer and waits for a handler already running on another
    /// CPU to finish. Returns true if the handler did not and will not run.
    ///
    /// Called from inside the timer's own handler it cannot wait, and does
    /// not: it clears the handler and returns false. Anything the calling
    /// thread holds that the handler needs must be taken by the handler
    /// through trylock_or_cancel(), or the two deadlock.
    auto cancel() -> bool;

    /// For handlers: spin on |lock| until it is taken (returns false, lock
    /// held) or until cancel() has marked this timer (returns true, lock not
    /// held -- the handler should return at once, because the thread that
    /// owns |lock| is waiting for it to).
    ///
    /// TA_TRY_ACQUIRE's first argument is the return value that means
    /// "acquired". Here that is false -- the inverse of a try_acquire() -- so
    /// the annotation has to say so, or the analysis reads the caller's
    /// `if (trylock_or_cancel(lock)) return;` as the success branch and
    /// reports the release that follows as releasing a lock never taken.
    TA_TRY_ACQUIRE(false, lock) auto trylock_or_cancel(sync::SpinLock &lock) -> bool;

    FORCE_INLINE
    auto on_expired() -> HandlerResult {
        DEBUG_ASSERT(m_on_expired);
        return m_on_expired(this);
    }

    FORCE_INLINE
    auto timer_queue() -> TimerQueue * {
        return m_queue.load(std::memory_order_relaxed);
    }

    [[nodiscard]]
    FORCE_INLINE
    auto deadline() const -> Deadline {
        return m_deadline;
    }

    [[nodiscard]]
    FORCE_INLINE
    auto clockid() const -> ClockId {
        return m_clockid;
    }

    [[nodiscard]]
    FORCE_INLINE
    auto is_cancelled() const -> bool {
        return m_cancelled.load(std::memory_order_acquire);
    }

    /// Whether the timer is in a queue's set of armed timers. Not whether its
    /// handler is running: a timer is unlinked just before its handler is
    /// called.
    [[nodiscard]]
    FORCE_INLINE
    auto is_linked() const -> bool {
        return m_hook.is_linked();
    }

    /// Queue order: by latest deadline, then by address. The address breaks
    /// ties because the queue is a set, not a multiset, and two timers due
    /// at the same instant are the common case (every sleep_for(10ms)
    /// issued in the same tick). Which of two equal timers runs first is
    /// immaterial.
    FORCE_INLINE
    friend auto operator<(Self const &x, Self const &y) noexcept -> bool {
        if (x.m_deadline.latest() != y.m_deadline.latest()) {
            return x.m_deadline.latest() < y.m_deadline.latest();
        }
        return &x < &y;
    }

  private:
    friend class TimerQueue;

    gktl::Canary m_canary;
    ClockId m_clockid;
    std::atomic<TimerQueue *> m_queue;
    Deadline m_deadline;
    Handler m_on_expired;
    std::atomic<CpuNum> m_active_cpu;
    /// See the class comment. The timer's one yes/no fact, so it has no word
    /// to share; an atomic of its own because cancel() sets it with no lock
    /// held while a handler on another CPU polls it.
    std::atomic<bool> m_cancelled;
    /// See the class comment. Guarded by the lock of m_queue, as the hook is.
    bool m_parked{false};
    /// What puts the timer in a set. A member and not a base class: a Timer is
    /// not a hook, it has one. An any hook, so the same member serves a
    /// container of another kind (a list of timers to move to another CPU, say)
    /// without a second one beside it; the timer is in one at a time.
    ustl::collections::intrusive::AnyMemberHook<> m_hook;

  public:
    /// The option that names m_hook, for the set of armed timers a queue keeps
    /// (TimerQueue::ClockBase::active).
    using ActiveHook = ustl::collections::intrusive::AnyToSetHook<
        ustl::collections::intrusive::MemberHook<Self, decltype(m_hook), &Self::m_hook>>;
    using ActiveList = ustl::collections::intrusive::Set<Self, ActiveHook>;

    /// The same hook for the timers a pass has run and is to restart
    /// (TimerQueue::m_restart). Nothing there is ordered, so it is a singly
    /// linked list. Not a linear one: the last node of a linear list has a
    /// null link, which is what an unlinked node has, so is_linked() would
    /// answer false for it and the checks that trust it would miss it.
    using RestartHook = ustl::collections::intrusive::AnyToSlistHook<
        ustl::collections::intrusive::MemberHook<Self, decltype(m_hook), &Self::m_hook>>;
    using RestartList = ustl::collections::intrusive::Slist<Self, RestartHook>;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TIMER_HPP