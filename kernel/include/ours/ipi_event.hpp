/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IPI_EVENT_HPP
#define OURS_IPI_EVENT_HPP 1

#include <ours/types.hpp>

namespace ours {
enum class IpiEvent : u8 {
    /// "Run what I queued for you." The payload is not in the IPI -- an IPI
    /// carries nothing but its vector -- it is in the target's mailbox, which
    /// IpiService::sync_exec() filled in before sending this.
    Generic,
    Resched,
    Interrupt,
    Suspend,
    /// "Run your timer queue." What the broadcast timer device sends the CPUs
    /// it serves whose deadline has come (time::TickBroadcast).
    Timer,
};
} // namespace ours

#endif // OURS_IPI_EVENT_HPP
