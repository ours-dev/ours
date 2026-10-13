/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_EVENT_HPP
#define OURS_EVENT_HPP 1

#include <ours/status.hpp>
#include <ours/task/wait_queue.hpp>

#include <gktl/canary.hpp>
#include <ustl/option.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::sync {
/// A state threads wait on, one bit: signalled or not.
///
/// The operations are named after that bit: signal() sets it, waking whoever
/// waits, and unsignal() clears it, so that a later wait() blocks again.
/// They are the bit's two directions, which is why the opposite of signal()
/// is unsignal(). A Semaphore is the other model, a count: its post() adds a
/// unit and its wait() takes one. post() is the counting form of signal(),
/// not its opposite. The difference shows when nobody waits: three posts are
/// kept for three later waits, while signals are not counted -- a
/// manual-reset event stays signalled through any number of waits until
/// unsignal(), and an AutoUnsignal one keeps at most one signal for the next
/// wait().
///
/// So an Event says "this has happened" (a transition is done, a handle has
/// closed, a queue has something in it), and a Semaphore says "one more of
/// these is ready" when each has to be taken exactly once.
class Event {
    using Self = Event;

  public:
    /// How an event behaves once signalled, chosen when it is made.
    ///
    /// None is a manual-reset event: a signal releases every waiter and stays
    /// standing until unsignal(), so every later wait() returns at once.
    ///
    /// AutoUnsignal hands each signal to exactly one waiter: signal() wakes
    /// one if any is parked and leaves nothing standing, or, with none parked,
    /// latches the signal for the next wait() to consume. An interrupt
    /// object's synchronous wait is built on one (InterruptDispatcher's
    /// m_event).
    ///
    /// This was a protected Flags that nothing could set: the constructor
    /// always chose None, so the AutoUnsignal branches in event.cpp were
    /// unreachable.
    enum class Options : u32 {
        None = 0,
        AutoUnsignal = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Options);

    /// A manual-reset event, not signalled.
    ///
    /// There was no constructor, so m_options was whatever the memory held:
    /// zero for the two events that live in zeroed per-CPU storage, garbage
    /// for any event on a stack -- and a garbage AutoUnsignal bit changes
    /// which waiters a signal() releases. object::wait_one() puts one on the
    /// stack for every wait.
    Event() : Event(Options::None) {}

    /// An event of the kind |options| names, not signalled.
    explicit Event(Options const options)
        : m_canary(this), m_result(), m_options(options), m_wq() {}

    auto signal(Status = Status::Ok) -> void;

    auto unsignal() -> void;

    auto wait() -> Status {
        return wait_etc(time::Deadline::infinite(), task::Interruptible::No,
                        task::ThreadSignals::None);
    }

    auto wait(time::Deadline const &deadline) -> Status {
        return wait_etc(deadline, task::Interruptible::Yes, task::ThreadSignals::None);
    }

    auto wait(time::Deadline const &deadline, task::ThreadSignals sigmask) -> Status {
        return wait_etc(deadline, task::Interruptible::Yes, sigmask);
    }

    /// A deadline the caller may choose not to be interrupted in: a join
    /// waits out its deadline whatever signals the joiner gets
    /// (Thread::join() blocks with Interruptible::No).
    auto wait(time::Deadline const &deadline, task::Interruptible interruptible) -> Status {
        return wait_etc(deadline, interruptible, task::ThreadSignals::None);
    }

  protected:
    auto wait_etc(time::Deadline, task::Interruptible interruptible, task::ThreadSignals sigmask)
        -> Status;

  private:
    gktl::Canary m_canary;
    ustl::Option<Status> m_result;
    Options const m_options;
    task::WaitQueue m_wq;
};

} // namespace ours::sync

#endif // #ifndef OURS_EVENT_HPP