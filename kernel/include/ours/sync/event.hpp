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
    /// A manual-reset event, not signalled.
    ///
    /// There was no constructor, so m_flags was whatever the memory held: zero
    /// for the two events that live in zeroed per-CPU storage, garbage for any
    /// event on a stack -- and a garbage AutoUnsignal bit changes which
    /// waiters a signal() releases. object::wait_one() puts one on the stack
    /// for every wait.
    Event() : m_canary(this), m_result(), m_flags(Flags::None), m_wq() {}

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
    enum class Flags {
        None,
        AutoUnsignal = BIT(0),
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    auto wait_etc(time::Deadline, task::Interruptible interruptible, task::ThreadSignals sigmask)
        -> Status;

  private:
    gktl::Canary m_canary;
    ustl::Option<Status> m_result;
    Flags m_flags;
    task::WaitQueue m_wq;
};

} // namespace ours::sync

#endif // #ifndef OURS_EVENT_HPP