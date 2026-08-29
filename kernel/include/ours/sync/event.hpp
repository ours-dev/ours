/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_EVENT_HPP
#define OURS_EVENT_HPP 1

#include <ours/task/wait_queue.hpp>
#include <ours/status.hpp>

#include <gktl/canary.hpp>

#include <ustl/option.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::sync {
    class Event {
        using Self = Event;
      public:
        auto signal(Status = Status::Ok) -> void;

        auto unsignal() -> void;

        auto wait() -> Status {
            return wait_etc(time::Deadline::infinite(), task::Interruptible::No, Signals::None);
        }

        auto wait(time::Deadline const &deadline) -> Status {
            return wait_etc(deadline, task::Interruptible::Yes, Signals::None);
        }

        auto wait(time::Deadline const &deadline, Signals sigmask) -> Status {
            return wait_etc(deadline, task::Interruptible::Yes, sigmask);
        }
      protected:
        enum class Flags {
            None,
            AutoUnsignal = BIT(0),
        };
        USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

        auto wait_etc(time::Deadline, task::Interruptible interruptible, Signals sigmask) -> Status;
      private:
        gktl::Canary m_canary;
        ustl::Option<Status> m_result;
        Flags m_flags;
        task::WaitQueue m_wq;
    };

} // namespace ours::sync

#endif // #ifndef OURS_EVENT_HPP