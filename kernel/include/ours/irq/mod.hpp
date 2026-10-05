/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_MOD_HPP
#define OURS_IRQ_MOD_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <ktl/result.hpp>
#include <ustl/option.hpp>
#include <ustl/views/span.hpp>

namespace arch {
struct IrqFrame;
}

namespace ours::irq {
class IrqObserver;
struct IrqChip;

/// Builds the table of interrupt lines: one line for each number in
/// `hirqnums`, named by that number, so a line's VIrqNum is its HIrqNum. On
/// x86 that number is the GSI, which is also what dispatch delivers, since
/// the platform programs each GSI with the vector PlatformIrqMin + GSI.
///
/// Every line is bound to `chip`, the controller that drives its pin, which
/// mask(), unmask() and configure() go through. The chip outlives the table.
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
auto init_lines(ustl::views::Span<HIrqNum const> hirqnums, IrqChip const &chip) -> Status;

/// Hangs `observer` on line `virqnum`; from then on every interrupt on the
/// line calls the observer's handler with the observer's context. The caller
/// owns the observer and keeps it alive until detach() has returned.
///
/// An observer that says Permanent takes the line for good: no other attach
/// succeeds after it, and detach() refuses it.
///
/// AlreadyBound if the line is Permanent, or holds an observer and the two do
/// not both say Shared, or holds one and this observer says Permanent;
/// BadState if the observer is on a line already, NoResource if there is no
/// such line, InvArgs for INVALID_V_IRQ_NUM or a null handler.
[[nodiscard]]
auto attach(VIrqNum virqnum, IrqObserver &observer) -> Status;

/// Takes `observer` off the line it hangs on; NotFound if it hangs on none,
/// AlreadyBound if the line is Permanent.
///
/// A barrier: it returns only once no handler of the line is running on any
/// CPU, so the owner may free the observer and its context right after. It
/// spins with interrupts off while a handler runs, so it must not be called
/// from a handler of the same line.
[[nodiscard]]
auto detach(IrqObserver &observer) -> Status;

/// Masks line `virqnum` at its controller, so the pin raises no interrupt
/// until unmask().
///
/// Callable from any context, a handler of the same line included: it takes
/// no line lock, only the controller's register lock. It does not wait for
/// a handler already running on another CPU; detach() is the barrier.
///
/// NoResource if there is no such line, InvArgs for INVALID_V_IRQ_NUM,
/// NotSupported if the line's controller cannot mask.
[[nodiscard]]
auto mask(VIrqNum virqnum) -> Status;

/// Unmasks line `virqnum` at its controller.
/// The same context rules and answers as mask().
[[nodiscard]]
auto unmask(VIrqNum virqnum) -> Status;

/// Sets how line `virqnum` signals, and leaves it masked: the order is
/// configure, attach, unmask. Only a line nobody is on may change how it
/// signals: an observer already there would be handed interrupts it was not
/// set up for, or none.
///
/// Thread context only: it takes the line's lock. AlreadyBound if the line
/// has an observer, the same answers as mask() otherwise.
[[nodiscard]]
auto configure(VIrqNum virqnum, TriggerMode trigger, Polarity polarity) -> Status;

/// How line `virqnum` signals now: what configure() last set, or what the
/// platform programmed at boot from the firmware's tables. An owner that
/// leaves the mode as it is reads it here to know whether the line is
/// level-triggered, and so has to stay masked from an interrupt until the
/// device behind it has been serviced.
///
/// Callable from any context, as mask() is: it reads the controller under
/// the controller's register lock and takes no line lock. The same answers
/// as mask(), NotSupported meaning the controller cannot say.
[[nodiscard]]
auto trigger_mode(VIrqNum virqnum) -> ktl::Result<TriggerMode>;

auto set_affinity(VIrqNum virqnum, CpuMask const &targets) -> Status;
auto handle_irq_generic(VIrqNum virqnum) -> IrqReturn;

/// How many interrupts on line `virqnum` no observer handled since boot;
/// none if there is no such line.
[[nodiscard]]
auto spurious_count(VIrqNum virqnum) -> ustl::Option<u64>;

auto handle_irq_preamble(HIrqNum irqnum, ::arch::IrqFrame *frame) -> void;
auto handle_irq_postamble() -> bool;

/// Trap frame of the interrupt currently handled on this CPU, or nullptr
/// outside interrupt context.
auto current_frame() -> ::arch::IrqFrame *;
} // namespace ours::irq

#endif // OURS_IRQ_MOD_HPP
