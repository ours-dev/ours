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
class Event {
    using Self = Event;

  public:
    /// How an event behaves once signalled, chosen when it is made. Zircon's
    /// Event::Flags, whose one flag, AUTOUNSIGNAL, its constructor takes
    /// (kernel/include/kernel/event.h).
    ///
    /// None is a manual-reset event: a signal releases every waiter and stays
    /// standing until unsignal(), so every later wait() returns at once.
    ///
    /// AutoUnsignal hands each signal to exactly one waiter: signal() wakes
    /// one if any is parked and leaves nothing standing, or, with none parked,
    /// latches the signal for the next wait() to consume. Zircon's
    /// AutounsignalEvent; an interrupt object's synchronous wait is built on
    /// one (Zircon interrupt_dispatcher.h, event_).
    ///
    /// This was a protected Flags that nothing could set: the constructor
    /// always chose None, so the AutoUnsignal branches in event.cpp were
    /// unreachable.
    enum class Options : u32 {
        None = 0,
        AutoUnsignal = BIT(0),
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
    /// waits out its deadline whatever signals the joiner gets, as Zircon's
    /// TaskState::Join() blocks with Interruptible::No.
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