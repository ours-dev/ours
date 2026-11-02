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
class DescTable;
class DomainSlot;

/// The machine's directory of interrupt domains (DomainRegistry), what every
/// function below resolves its numbers through. A control call opens a short
/// read of the directory (a task::IrqRcuReadGuard, interrupts off) and holds
/// it to the last use of the line, so a domain being taken away is never half
/// seen; nothing in the section sleeps, allocates or copies from user memory.
/// The interrupt path does not read the directory: the root handler loads its
/// domain's table from the DomainSlot the directory published it into, and
/// hands it to handle_irq_generic().
///
/// Registers the root controller's lines as the machine's first domain: one
/// line for each hardware number in `hirqnums`, in a table of its own, held
/// by the directory under DomainId::Root and published into `slot`, which the
/// root handler reads for every interrupt. On x86 that number is the GSI,
/// which is also what dispatch delivers, since the platform programs each GSI
/// with the vector PlatformIrqMin + GSI. A line is named by its virq, which
/// the domain assigns (Domain) and which is never the hardware number:
/// virq_of_hwirq() and hwirq_of_virq() go between the two.
///
/// Every line is bound to `chip`, the controller that drives its pin, which
/// mask(), unmask() and configure() go through, and which ends each of the
/// line's interrupts. The chip outlives the domain. Each line takes the flow
/// that suits how the chip says its pin signals: the level flow for a level
/// pin, the edge flow otherwise.
///
/// The platform calls it once, after it has read its interrupt controllers
/// from the firmware and programmed the lines it found, and before it unmasks
/// any of them, and before it installs the handler that reads `slot`. Until
/// then there are no lines: attach() answers NoResource, and an interrupt
/// finds no line and is ended by its caller (handle_irq_generic() answers
/// false on a null table).
///
/// AlreadyExists if the root domain is registered already, BadState if `slot`
/// holds a table, InvArgs for an invalid hardware number, an empty list or a
/// chip with no eoi or no mask, NoMemory.
/// Nothing is kept from a call that fails.
[[nodiscard]]
auto init_lines(ustl::views::Span<HIrqNum const> hirqnums, Chip const &chip, DomainSlot &slot)
    -> Status;

/// Hangs `action` on line `virqnum`; from then on every interrupt on the
/// line calls the action's handler with the action's context. The caller
/// owns the action and keeps it alive until detach() has returned.
///
/// An action that says Permanent takes the line for good: no other attach
/// succeeds after it, and detach() refuses it.
///
/// An action that says Oneshot makes the line Oneshot for as long as it is
/// on it. On a level line, the line's flow then masks the line before the
/// handler runs, and keeps it masked from an interrupt the handler answers
/// WakeThread for until the action's finalize_oneshot(); an edge line
/// ignores it. Oneshot never shares and is never Permanent: the line keeps
/// one record of what it is held for, under its lock.
///
/// AlreadyBound if the line is Permanent, or holds an action and the two do
/// not both say Shared, or holds one and this action says Permanent;
/// BadState if the action is on a line already, NoResource if there is no
/// such line or the kernel is taking it away (a removal of its domain has
/// marked it, and the answer clears if the removal is refused), InvArgs for
/// INVALID_V_IRQ_NUM, a null handler, or an action
/// that says Oneshot together with Permanent or Shared; NotSupported for a
/// Oneshot action on a line whose controller cannot mask and unmask, or
/// cannot say how its pins signal (give a mode: the attach with a mode),
/// which is answered before AlreadyBound.
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
/// AlreadyBound if the line is Permanent. NotFound also answers an action the
/// kernel took off (Desc::revoke()), and then means the removal is done with
/// the action and what it is embedded in, so the owner may free it as it may
/// after Ok.
///
/// A barrier: it returns only once no handler of the line is running on any
/// CPU, so the owner may free the action and its context right after. It
/// spins with interrupts off while a handler runs, so it must not be called
/// from a handler of the same line.
///
/// A Oneshot line stops being held as its action goes, without waiting for a
/// finalize_oneshot() that may never come; when the last action goes, the
/// line is left masked until the next owner unmasks it.
[[nodiscard]]
auto detach(Action &action) -> Status;

/// Tells line `virqnum` that `action`'s thread has serviced its device: the
/// line stops being held and is unmasked. Called by the action's owner when
/// the work its handler answered WakeThread for is done, and only then: on
/// a level line, an unmask before the device is serviced raises the same
/// interrupt again at once.
///
/// Does nothing if `action` is no longer on that line -- a detach that ran
/// first, or a removal, has cleared held and masked the line for it -- or if
/// the line is not Oneshot, or if there is no such line. A line marked for
/// removal is finalized as any other. The owner names the line
/// because the action's own record of it is written under that line's lock,
/// and a finalize may race a detach of the same action.
///
/// It takes the line's lock, which a dispatch holds while it calls the
/// handler. So it is never called from a handler of the same line, which
/// would spin on its own dispatch, nor with a lock held that the handler
/// takes: a dispatch takes that lock inside the line's, and this would take
/// the two the other way round. Nor from a handler of another line, whose
/// dispatch holds that line's lock: no order is set between two lines'
/// locks.
auto finalize_oneshot(VIrqNum virqnum, Action const &action) -> void;

/// Masks line `virqnum` at its controller, so the pin raises no interrupt
/// until it is unmasked.
///
/// It takes the line's lock, then the controller's register lock, as every
/// other write of the line's mask does, so it lands wholly before or wholly
/// after a dispatch of the line, and waits for one running on another CPU;
/// a Permanent line's dispatch takes no line lock, so there it waits for
/// none. The rules of finalize_oneshot() follow from that: never from a
/// handler of the same line, which would spin on its own dispatch, nor with
/// a lock held that the line's handler takes, which would take the two locks
/// the other way round from a dispatch, nor from a handler of another line:
/// its dispatch holds that line's lock, and two lines' locks have no order,
/// so two handlers each masking the other's line on two CPUs would wait on
/// each other for good. Thread context.
///
/// A mask is not a disable: the line keeps no record of it, so the next
/// unmask opens the line again, whoever makes it -- unmask(),
/// finalize_oneshot(), or a Oneshot level line's flow, after an interrupt
/// that was on its way when the mask landed and that its action handled
/// itself.
///
/// NoResource if there is no such line, InvArgs for INVALID_V_IRQ_NUM,
/// NotSupported if the line's controller cannot mask. A line the kernel is
/// taking away is masked as any other.
[[nodiscard]]
auto mask(VIrqNum virqnum) -> Status;

/// Unmasks line `virqnum` at its controller, unless the line is held: a
/// Oneshot line whose thread still owes its device a service stays masked,
/// and finalize_oneshot() unmasks it once the thread is done. Answers Ok
/// either way, since the line runs again as soon as nothing is owed. A line
/// the kernel is taking away is not started: NoResource.
///
/// Held is read under the line's lock, in the same hold as the unmask, so a
/// dispatch on another CPU cannot hold the line between the two. The owner
/// of a Oneshot action may call it to start the line after attach() even if
/// an interrupt has held the line in between.
///
/// The same context rules and answers as mask(), NotSupported meaning the
/// controller cannot unmask.
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
/// has an action, NoResource also if the kernel is taking the line away, the
/// same answers as mask() otherwise.
[[nodiscard]]
auto configure(VIrqNum virqnum, TriggerMode trigger, Polarity polarity) -> Status;

/// How line `virqnum` signals now: what configure() last set, or what the
/// platform programmed at boot from the firmware's tables. An owner that
/// leaves the mode as it is reads it here to know whether the line is
/// level-triggered, and so has to stay masked from an interrupt until the
/// device behind it has been serviced.
///
/// Callable from any context, a handler of the same line included: it reads
/// the controller under the controller's register lock and takes no line
/// lock. The same answers as mask(), NotSupported meaning the controller
/// cannot say.
[[nodiscard]]
auto trigger_mode(VIrqNum virqnum) -> ktl::Result<TriggerMode>;

/// Whether line `virqnum` is masked at its controller now, read from the
/// controller. For kernel tests: the irq layer keeps no copy of a line's
/// mask and nothing on the interrupt path reads it, so this is how a test
/// sees what a line's flow, or the owner on it, left at the controller. The
/// answer holds only while nothing else masks or unmasks the line, so a test
/// reads it where it alone drives the line.
///
/// Callable from any context, as trigger_mode(): it reads the controller
/// under the controller's register lock and takes no line lock. The same
/// answers as mask(), NotSupported meaning the controller cannot say.
[[nodiscard]]
auto is_masked_for_test(VIrqNum virqnum) -> ktl::Result<bool>;

/// Aims line `virqnum` at the CPUs in `targets`. NotSupported for every line:
/// no controller moves a line yet, and each line stays on the CPU the platform
/// aimed it at. The call, including what it returns, is settled by the work of
/// interrupt-system P4.
auto set_affinity(VIrqNum virqnum, CpuMask const &targets) -> Status;

/// The resolver of the user ABI: the virq of the line made for hardware
/// number `hirqnum` in the domain named `id`; INVALID_V_IRQ_NUM if no such
/// domain is registered or it holds no such line. interrupt_map asks it with
/// the domain and the source the route service's answer named. The number is
/// good for good: virqs are not reused, so it is safe to keep after the
/// lookup, and never equal to `hirqnum`, and not the same on another machine
/// or another kernel: a program asks, and does not compute it.
[[nodiscard]]
auto virq_of_domain(DomainId id, HIrqNum hirqnum) -> VIrqNum;

/// The same question asked of the root controller's domain, whose numbers are
/// the platform's single numbering (on x86, the GSI): what the kernel's own
/// code names a root line by. The lines of a second domain are asked for by
/// domain and hardware number together (virq_of_domain()), never by the
/// number alone, since two domains may hold the same one.
[[nodiscard]]
auto virq_of_hwirq(HIrqNum hirqnum) -> VIrqNum;

/// The hardware number of line `virqnum`, the inverse of virq_of_hwirq();
/// INVALID_H_IRQ_NUM if there is no such line.
[[nodiscard]]
auto hwirq_of_virq(VIrqNum virqnum) -> HIrqNum;

/// The interrupt that arrived for the line made for hardware number
/// `hirqnum`, in interrupt context: runs the line's flow, which calls its
/// actions and then ends the interrupt through the line's chip; a line
/// nobody is on is masked, and its interrupt ended. Answers false if there is
/// no such line: nothing has ended the interrupt then, and the caller, which
/// knows how the interrupt arrived, ends it itself. Either way it is ended
/// exactly once.
///
/// The lines are those of `table`, the handler's own domain, which the
/// handler loads from the DomainSlot the directory published its table into:
/// one acquire load, and no read of the directory on the interrupt path. A
/// null table answers false, the shape of a domain that is gone, whose
/// handler ends what it receives itself.
///
/// By hardware number, not virq: it is what the interrupt entry has, and it
/// indexes the table directly, with no lookup between.
[[nodiscard]]
auto handle_irq_generic(DescTable *table, HIrqNum hirqnum) -> bool;

/// The table of the root controller's domain, what a root handler holds its
/// DomainSlot for and what handle_irq_generic() dispatches through. Null
/// before init_lines(). For a kernel test that plays an interrupt by hardware
/// number, as pinlines.test does for a number no pin has; everything else
/// names a line by its virq. The root domain is never removed, so the
/// pointer, once it is not null, is good for good.
[[nodiscard]]
auto root_table_for_test() -> DescTable *;

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

/// What the removal of a domain does to one of its lines (DescTable::
/// revoke_line()): marks the line and takes its actions off it, telling
/// each through its cancel hook; AlreadyBound with nothing changed for a
/// Permanent line or an action with no hook, NoResource for no such line.
/// Nothing calls it but tests yet: no tree code removes a domain.
/// unmark_dying_for_test() makes the line take actions again.
[[nodiscard]]
auto revoke_line_for_test(VIrqNum virqnum) -> Status;
auto unmark_dying_for_test(VIrqNum virqnum) -> void;

/// How many interrupts on line `virqnum` no action handled since boot;
/// none if there is no such line.
[[nodiscard]]
auto spurious_count(VIrqNum const virqnum) -> ustl::Option<u64>;

/// Whether the kernel itself holds line `virqnum`: an action that says
/// Permanent is on it, as the legacy timer's is. False for a line nobody is on,
/// a line a user program's interrupt object holds, and a number that is no
/// line. A line is Permanent for good once it is, and no user program can make
/// one; the kernel makes them as it starts, and an answer asked for before it
/// has taken its own line can be out of date. So the answer chooses which
/// refusal a program gets and never what it is granted: attach() is the
/// authority, and refuses a Permanent line with AlreadyBound whatever was asked
/// first. A kernel driver that takes a line for its own uses a Permanent
/// action, and so keeps programs off it: interrupt_map and interrupt_create
/// answer AccessDenied for it. A Permanent action cannot be Oneshot or Shared
/// and is never detached, so a kernel holder that needs any of that is not
/// reported.
[[nodiscard]]
auto is_kernel_owned(VIrqNum virqnum) -> bool;

/// The bookkeeping on entry to a trap, which arch calls once, before it
/// dispatches: records the frame and raises the counts that keep this CPU from
/// switching threads until the exit. Exceptions take it too, a page fault
/// among them.
auto handle_irq_preamble(::arch::IrqFrame *frame) -> void;

/// The bookkeeping on exit from a trap, which arch calls once, after the
/// dispatch. Answers whether the CPU should switch threads before returning
/// to the interrupted context; the switch is arch's.
auto handle_irq_postamble() -> bool;

/// The function the root interrupt controller's driver gives arch. Arch calls
/// it for an interrupt that comes through the root controller, after it has
/// separated the causes it can tell apart without the controller (x86's
/// vector, a RISC-V trap cause: the local timer and the inter-processor
/// interrupts) and before the postamble. Where only the controller's own read
/// tells them apart (an acknowledge read returns the processor-local
/// interrupts too), the handler hands those back to arch. The handler finds
/// out which interrupt it is in the way its controller allows (a vector, an
/// acknowledge read, a claim), hands it to handle_irq_generic(), and ends the
/// interrupt itself if no line had it, since nothing else knows how.
using RootHandler = auto (*)(::arch::IrqFrame *frame) -> void;

/// Installs `handler` as the root handler. There is one for the kernel, and it
/// is installed once, before the first interrupt of the root controller can be
/// taken: the controller's driver does it when it starts, ahead of the
/// instruction that enables interrupts. A second call is a kernel bug.
auto set_root_handler(RootHandler handler) -> void;

/// Calls the root handler, in interrupt context. An interrupt that arrives
/// before one is installed is a kernel bug.
auto handle_root_irq(::arch::IrqFrame *frame) -> void;

/// Trap frame of the interrupt currently handled on this CPU, or nullptr
/// outside interrupt context.
auto current_frame() -> ::arch::IrqFrame *;
} // namespace ours::irq

#endif // OURS_IRQ_MOD_HPP
