/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/wait.hpp>
#include <ours/status.hpp>

namespace ours::syscall {
auto object_wait_one(HandleValue const handle, Signals const signals, Instant const deadline,
                     user_copy::UserOutPtr<Signals> const observed) -> Status {
    Signals current = Signals::None;
    auto const status = object::wait_one(handle, signals, deadline, &current);

    // What was asserted is reported for every way a wait can end -- matched,
    // timed out, or cancelled by a close -- since each is a real answer about
    // the object. Not for a wait that never started (a bad handle, a missing
    // right), where there is no object to report on. Optional, as in Zircon.
    auto const ended =
        status == Status::Ok || status == Status::TimeOut || status == Status::Cancelled;
    if (ended && !observed.is_null()) {
        if (auto const copied = observed.copy_to_user(current); copied != Status::Ok) {
            return copied;
        }
    }
    return status;
}
} // namespace ours::syscall
