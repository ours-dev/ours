/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_MOD_HPP
#define OURS_IRQ_MOD_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <ustl/views/span.hpp>

namespace arch {
struct IrqFrame;
}

namespace ours::irq {
class IrqObserver;

/// Builds the table of interrupt lines: one line for each number in
/// `hirqnums`, named by that number, so a line's VIrqNum is its HIrqNum. On
/// x86 that number is the GSI, which is also what dispatch delivers, since
/// the platform programs each GSI with the vector PlatformIrqMin + GSI.
///
/// The platform calls it once, after it has read its interrupt controllers
/// from the firmware and programmed the lines it found, and before it unmasks
/// any of them. Until then there are no lines: attach() answers NoResource
/// and an interrupt finds nothing to notify.
///
/// BadState if the table is built already, InvArgs for INVALID_V_IRQ_NUM or
/// an empty list, AlreadyExists if a number is given twice, NoMemory.
/// Nothing is kept from a call that fails.
[[nodiscard]]
auto init_lines(ustl::views::Span<HIrqNum const> hirqnums) -> Status;

/// Hangs `observer` on line `virqnum`; from then on every interrupt on the
/// line calls the observer's handler with the observer's context. The caller
/// owns the observer and keeps it alive until detach() has returned.
///
/// AlreadyBound if the line holds an observer and the two do not both say
/// Shared, BadState if the observer is on a line already, NoResource if
/// there is no such line, InvArgs for INVALID_V_IRQ_NUM or a null handler.
[[nodiscard]]
auto attach(VIrqNum virqnum, IrqObserver &observer) -> Status;

/// Takes `observer` off the line it hangs on; NotFound if it hangs on none.
///
/// Not yet a barrier: the line has no lock, so a handler already running on
/// another CPU may still be running, or still walking the line's list, when
/// this returns. Nothing calls it until the line has that lock.
[[nodiscard]]
auto detach(IrqObserver &observer) -> Status;

auto set_affinity(VIrqNum virqnum, CpuMask const &targets) -> Status;
auto handle_irq_generic(VIrqNum virqnum) -> IrqReturn;
auto handle_irq_preamble(HIrqNum irqnum, arch::IrqFrame *frame) -> void;
auto handle_irq_postamble() -> bool;

/// Trap frame of the interrupt currently handled on this CPU, or nullptr
/// outside interrupt context.
auto current_frame() -> arch::IrqFrame *;
} // namespace ours::irq

#endif // OURS_IRQ_MOD_HPP
