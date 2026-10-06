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
class Action;
struct Chip;

/// Builds the table of interrupt lines: one line for each number in
/// `hirqnums`, named by that number, so a line's VIrqNum is its HIrqNum. On
/// x86 that number is the GSI, which is also what dispatch delivers, since
/// the platform programs each GSI with the vector PlatformIrqMin + GSI.
///
/// Every line is bound to `chip`, the controller that drives its pin, which
/// mask(), unmask() and configure() go through, and which ends each of the
/// line's interrupts. The chip outlives the table. Each line takes the flow
/// that suits how the chip says its pin signals: the level flow for a level
/// pin, the edge flow otherwise.
///
/// The platform calls it once, after it has read its interrupt controllers
/// from the firmware and programmed the lines it found, and before it unmasks
/// any of them. Until then there are no lines: attach() answers NoResource,
/// and an interrupt finds no line and is ended by its caller
/// (handle_irq_generic() answers false).
///
/// BadState if the table is built already, InvArgs for INVALID_V_IRQ_NUM,
/// an empty list or a chip with no eoi or no mask, AlreadyExists if a number
/// is given twice, NoMemory.
/// Nothing is kept from a call that fails.
[[nodiscard]]
auto init_lines(ustl::views::Span<HIrqNum const> hirqnums, Chip const &chip) -> Status;

/// Hangs `action` on line `virqnum`; from then on every interrupt on the
/// line calls the action's handler with the action's context. The caller
/// owns the action and keeps it alive until detach() has returned.
///
/// An action that says Permanent takes the line for good: no other attach
/// succeeds after it, and detach() refuses it.
///
/// AlreadyBound if the line is Permanent, or holds an action and the two do
/// not both say Shared, or holds one and this action says Permanent;
/// BadState if the action is on a line already, NoResource if there is no
/// such line, InvArgs for INVALID_V_IRQ_NUM or a null handler.
[[nodiscard]]
auto attach(VIrqNum virqnum, Action &action) -> Status;

/// Hangs `action` on line `virqnum` as attach() does, but only on a line
/// nobody is on, and settles how the line signals in the same step: with
/// `mode` it sets the line to that mode, as configure() would; with none it
/// leaves the line as it is and reads how it signals, as trigger_mode()
/// would. Answers the line's trigger mode, which holds for as long as the
/// action is on the line, since configure() refuses a line someone is on.
///
/// The line's lock is held from the check that the line is free, through the
/// mode, to the link, so no other caller's mode can land in between. Set with
/// configure() and then attach(), two owners racing for one free line could
/// each set or read its mode before either took it, and the one that took it
/// would hold a line in the other's mode: an owner that masks a level line
/// until it is serviced, or leaves an edge line unmasked, would be doing so
/// on the wrong kind of line.
///
/// Leaves the line masked, whichever way the mode was settled: the caller
/// unmasks it once it is ready for the first interrupt, which it can only be
/// once it knows how the line signals. The line's flow follows the mode, set
/// or read, before the action is linked.
///
/// Thread context only, as configure(). AlreadyBound if the line has an
/// action or is Permanent; NotSupported if the line's controller cannot
/// set its mode, or cannot say what it is and mask it; the other answers as
/// attach().
[[nodiscard]]
auto attach(VIrqNum virqnum, Action &action, ustl::Option<LineMode> mode)
    -> ktl::Result<TriggerMode>;

/// Takes `action` off the line it hangs on; NotFound if it hangs on none,
/// AlreadyBound if the line is Permanent.
///
/// A barrier: it returns only once no handler of the line is running on any
/// CPU, so the owner may free the action and its context right after. It
/// spins with interrupts off while a handler runs, so it must not be called
/// from a handler of the same line.
[[nodiscard]]
auto detach(Action &action) -> Status;

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

/// Sets how line `virqnum` signals, and leaves it masked; the line's flow
/// follows the new mode. Only a line nobody is on may change how it signals:
/// an action already there would be handed interrupts it was not set up for,
/// or none.
///
/// For a line nobody is about to take. An owner sets the mode of the line it
/// takes through attach() with a mode, which does both under one hold of the
/// line's lock: between this and a later attach(), another caller can set
/// the mode again.
///
/// Thread context only: it takes the line's lock. AlreadyBound if the line
/// has an action, the same answers as mask() otherwise.
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

/// The interrupt that arrived for line `virqnum`, in interrupt context: runs
/// the line's flow, which calls its actions and then ends the interrupt
/// through the line's chip; a line nobody is on is masked, and its interrupt
/// ended. Answers false if there is no such line: nothing has ended the
/// interrupt then, and the caller, which knows how the interrupt arrived,
/// ends it itself. Either way it is ended exactly once.
[[nodiscard]]
auto handle_irq_generic(VIrqNum virqnum) -> bool;

/// The flow of line `virqnum` for a kernel test that plays an interrupt from
/// thread context: the same actions under the same locks as
/// handle_irq_generic(), without the end of interrupt, since no interrupt is
/// in service to end. A line nobody is on is masked at its controller all the
/// same, as a real interrupt would leave it. Answers what the actions
/// answered, None if there is no such line. Preemption is the caller's: a
/// dispatch whose handler wakes a thread needs it off, as interrupt context
/// has it.
[[nodiscard]]
auto handle_irq_for_test(VIrqNum virqnum) -> IrqReturn;

/// How many interrupts on line `virqnum` no action handled since boot;
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
