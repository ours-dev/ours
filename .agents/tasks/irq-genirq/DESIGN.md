# irq-genirq: rebuild kernel/irq around line descriptors and flow handlers, with ONESHOT masking owned by the line and the interrupt object as the threaded handler

**Date**: 2026-10-05
**Status**: Proposed

## Problem

Paths: ours paths are relative to the repository root. `linux/` is `~/Workspaces/projects/linux` at 62cc90241548, and `zircon/` is `~/Workspaces/projects/fuchsia/zircon`.

- **No flow handlers, no line state.**
  - The EOI is hard-coded in arch after the handler, with a TODO to move it into the chip (`kernel/arch/x86/kernel/faults.cpp:242-246`).
  - The chip's `issue_eoi`, `issue_ack`, `startup` and `set_affinity` are never called (`kernel/arch/x86/apic/io_apic.cpp:138-157`).
  - `issue_ack` calls the directed IO-APIC EOI, which writes the GSI where the vector belongs (`io_apic.cpp:151-153`; `docs/repairs/261003/x86-apic.md:23-35`).
- **The level mask lives in the object, and it writes the controller without the line's lock.**
  - The object masks and unmasks in four places (`kernel/object/interrupt_dispatcher.cpp:41-44`, `:238-240`, `:263-265`, `:409-411`).
  - `irq::mask` and `irq::unmask` take no L1, because a handler masks its own line (`kernel/include/ours/irq/irq_object.hpp:78-82`).
  - The HPET masks the PIT's line the same way (`kernel/arch/x86/kernel/time/hpet.cpp:317-338`).
- **Two scaffolds stand empty.** `domain.hpp` has 0 bytes and `class Action {}` is empty (`kernel/include/ours/irq/action.hpp:10-11`). Both are planned work, not dead code (`.claude/skills/ours-kernel-design/SKILL.md:49-50`).
- **The user's question.** The user chose to rewrite kernel/irq in genirq style, and asked whether some of its responsibilities could move into the interrupt objects. The answer is that nothing new moves in:
  - The objects already hold the threaded-handler half: deferred delivery, the ack, and coalescing.
  - The level mask should move out of the objects and into the line's flow.
- **Two coherent models.** Neither is a stand-in for the other:
  - **Zircon: the mask is a property of the object kind.** The event object sets the mask flags for level lines (`zircon/kernel/object/interrupt_event_dispatcher.cc:72-74`). The MSI object always masks, inside the device (`zircon/kernel/object/include/object/msi_interrupt_dispatcher.h:72-74`). The kernel layer has one fixed path: handler, then EOI (`zircon/kernel/platform/pc/interrupts.rs:224-230`).
  - **Linux: the mask is a property of the line's flow.** `handle_fasteoi_irq` masks an ONESHOT line (`linux/kernel/irq/chip.c:765-766`). A chip can declare itself one-shot safe, and then ONESHOT is dropped (`linux/kernel/irq/manage.c:1545-1554`). PCI-MSI declares this (`linux/drivers/pci/msi/irqdomain.c:133`).
- **This revisits two of the user's decisions of 2026-10-04** (`.agents/tasks/microkernel-interrupts/DESIGN.md:294-295`):
  - D1 kept Zircon's four pure virtuals so that MSI fits later. The recommended option keeps them.
  - D2 made a level line masked until the ack, "as Zircon". The recommended option keeps that behaviour and moves the act of masking into kernel/irq.

## Goals and non-goals

- **Goals:**
  - kernel/irq gets genirq's roles at "basically usable": descriptor, action, chip, data, the line table, and the `handle_fasteoi` and `handle_edge` flows.
  - Exactly one EOI per vector, sent through the chip.
  - ONESHOT, with one held bit per line and a finalize step.
  - Interrupt objects keep their state machine, NEEDACK, coalescing, port delivery and timestamps. They stop writing the controller mask.
  - Both images build and boot after every step, and each step is one commit.
- **Non-goals:**
  - A software Masked bit. The IO-APIC entry is the record. Linux's bit (`linux/kernel/irq/chip.c:443-452`) skips MMIO writes and supports lazy disable, and both are deferred.
  - Disable depth, lazy disable and edge resend (D5).
  - A per-action oneshot bitmap. Linux needs it only for shared threaded lines (`manage.c:1645-1690`); user lines are exclusive (`.agents/tasks/microkernel-interrupts/DESIGN.md:295`).
  - Stuck-line disabling.
  - irq_domain and dynamic vectors, until MSI (D4).
  - Affinity, and all performance work: vector matrix tuning, a shorter L1 hold, skipping the finalize on edge acks.
  - In-kernel interrupt threads (D3).
  - Shared lines for user objects.
  - Deleting `domain.hpp`, `XAPIC_CHIP` or any other scaffold.

## Design

### Objects

| Object | What it is | Linux | Zircon | ours today |
|---|---|---|---|---|
| `irq::Desc` | One per line: lock L1, its flow, its action list, the Oneshot flag, one held bit, and the count of interrupts no action handled | `struct irq_desc` (`linux/include/linux/irqdesc.h:81-135`) | `InterruptTableEntry`, a lock, a handler and a permanent flag per vector (`zircon/kernel/platform/pc/interrupt_manager.rs:58-70`) | `IrqObject` (`kernel/include/ours/irq/irq_object.hpp:42-142`) |
| `irq::Action` | One requester: handler, context, flags (Shared, Permanent, Oneshot) | `struct irqaction` (`linux/include/linux/interrupt.h:123-140`) | `interrupt_handler_t` (`zircon/kernel/dev/interrupt/include/dev/interrupt.h:24-35`) | `IrqObserver` (`kernel/include/ours/irq/irq_observer.hpp:33-91`); fills `action.hpp:10-11` |
| `irq::Chip` | Stateless controller operations: mask, unmask, configure, trigger_mode, eoi, and a test-only mask read (D10) | `struct irq_chip` (`linux/include/linux/irq.h:499-515`) | `mask_interrupt`, `unmask_interrupt` (`interrupt_manager.rs:200-210`) | `IrqChip` (`kernel/include/ours/irq/irq_chip.hpp:39-53`) |
| `irq::Data` | The line as its chip sees it: hwirq, virq, chip | `struct irq_data` (`linux/include/linux/irq.h:182-193`) | none | `IrqData` (`irq_chip.hpp:20-29`) |
| `handle_fasteoi`, `handle_edge` | The order of mask and EOI around the actions, chosen per line from its trigger mode (D8). Both send the EOI after the actions, so they differ only in the Oneshot mask | `handle_fasteoi_irq` (`linux/kernel/irq/chip.c:740-777`) and `handle_edge_irq` (`:827-862`), chosen by `mp_register_handler` (`linux/arch/x86/kernel/apic/io_apic.c:845-859`) | none: handler, then EOI, on every line (`interrupts.rs:224-230`) | none (`faults.cpp:242-246`) |
| `irq::DescTable` | Line number to Desc: a flat array, built once, read without a lock | `irq_to_desc()` (`linux/kernel/irq/irqdesc.c:412-415`); the `irq_desc[]` array without SPARSE_IRQ (`:583`) | none: its table is indexed by vector | `irq::IrqDispatcher` (`kernel/kernel/irq/irq_dispatcher.cpp:24-86`) |
| Vector to line | A vector arriving to a line number | per-CPU `vector_irq` (`linux/arch/x86/include/asm/hw_irq.h:126-127`; `linux/arch/x86/kernel/irq.c:228-235`) | the per-vector table | `faults.cpp` subtracts the base (`faults.cpp:21-25`) |
| `irq::Domain` (deferred) | A controller's hwirq space mapped into line numbers | `struct irq_domain` (`linux/include/linux/irqdomain.h:168`), ops (`:99`) | none | `domain.hpp`, 0 bytes |
| The interrupt object as the threaded handler | Delivers to its port or waiter; its ack, or the wait that serves as the ack, is the finalize | `irq_thread` and `irq_thread_fn` (`linux/kernel/irq/manage.c:1244-1286`, `:1141-1150`) | `InterruptHandler` and `AckInternal` (`zircon/kernel/object/interrupt_dispatcher.cc:205-257`, `:376-433`) | `InterruptEventDispatcher` (`kernel/object/interrupt_event_dispatcher.cpp:37-138`) |

Whether a pin is masked is recorded only in its IO-APIC entry. Every write is an idempotent read-modify-write under LI (`io_apic.cpp:83-93`), so kernel/irq keeps no copy of it.

### Events

1. **Boot.**
   - The platform programs every ISA line except the cascade masked, with vector 32+GSI (`kernel/platform/pc/interrupt.cpp:99-103`, `:112-141`), then builds the table (`:152`).
   - Each Desc gets the IO-APIC chip and the flow that matches the mode the pin was programmed with (D8). Linux makes the same choice in `mp_register_handler` (`io_apic.c:845-859`).
2. **Attach** (`interrupt_create`). It runs under one hold of L1, as today (`kernel/kernel/irq/irq_object.cpp:82-124`):
   - refuse an occupied line;
   - set or read the mode, and select the flow from it;
   - mask the line;
   - link the action and record Oneshot. Oneshot together with Permanent is refused.

   The caller then unmasks the line, as today (`kernel/include/ours/irq/mod.hpp:71-73`; `interrupt_event_dispatcher.cpp:93`). Linux instead starts the line inside `__setup_irq`, unless the caller passed `IRQF_NO_AUTOEN` (`manage.c:1754-1776`).
3. **A level interrupt.** The vector becomes a line number by subtraction (`faults.cpp:21-25`, `:242-243`), and `handle_fasteoi` runs under L1:
   - **No action:** mask, EOI, return (`chip.c:548-557`, `:758-761`). This is new: today a line with no action only counts the interrupt (`irq_object.cpp:201-202`).
   - **Oneshot:** mask (`chip.c:765-766`).
   - **Run the actions, still under L1** (D2 (a)). Linux drops its lock here (`linux/kernel/irq/handle.c:259-266`).
   - **The object's handler** takes L2, records the timestamp, and delivers or coalesces as today (`interrupt_dispatcher.cpp:211-249`).
   - **It always returns WakeThread.** After a delivery the object owes an ack. Once it is Destroyed, the line must stay masked until detach.
   - **WakeThread on a Oneshot line sets held.** With held clear, unmask (`chip.c:716-719`).
   - **EOI through the chip.** The IO-APIC chip's eoi is the LAPIC EOI, `apic_issue_eoi()` (`kernel/arch/x86/apic/local_apic.cpp:107-109`). Linux does the same in `ioapic_ack_level` (`io_apic.c:1655`, `:1704`, `:1858`).
4. **Ack, bound.**
   - Under L2, as today (`interrupt_dispatcher.cpp:394-424`): in NeedAck with an interrupt pending, the object sends the next packet and stays in NeedAck, and the line stays held. Otherwise it moves to Idle.
   - It drops L2. Only if it moved to Idle does it call `unmask_interrupt()`, which is `irq::finalize_oneshot(m_line, m_observer)`.
   - Finalize, under L1, returns if the action is no longer linked on that line. Otherwise it clears held and unmasks.
   - Zircon also unmasks after dropping the object lock, with `UNMASK_PREWAIT_UNLOCKED` (`zircon/kernel/object/include/object/interrupt_dispatcher.h:72-76`; `interrupt_dispatcher.cc:392-397`, `:429-431`).
   - Linux's `irq_finalize_oneshot` also spins on INPROGRESS and re-checks RUNTHREAD (`manage.c:1086-1134`, `:1110-1123`). Ours does neither; why is the user's exercise (Open questions).
5. **A wait that serves as the ack, unbound.**
   - `begin_wait` in NeedAck with nothing pending sets Waiting and `m_waiter` under L2 (`interrupt_dispatcher.cpp:108-121`, `:133-134`), and reports that it acked.
   - `wait()` calls `unmask_interrupt()` after L2 is dropped, before the `BEFORE_PARK_HOOK` and `park()` (`:53-61`).
6. **An edge interrupt.**
   - `handle_edge` runs under L1. On a Permanent line it runs without L1, and its flow then reads nothing L1 guards.
   - With no action: mask, EOI, return.
   - Otherwise it runs the actions and sends the EOI after them. Today and Zircon both order it this way (`faults.cpp:243-245`; `interrupts.rs:224-230`).
   - Oneshot is ignored, and held is never set.
   - Linux acks first (`chip.c:839-840`). It needs the PENDING replay loop (`:842-861`) because it drops the lock while the handlers run. With L1 held, a second edge waits for L1 instead.
7. **mask and unmask** (the HPET, an owner's first unmask, tests).
   - Through step 7 they stay as today: no L1, and idempotent at the register.
   - From step 8 they take L1, and unmask leaves a held line masked. That rule is ours's own. Linux's `irq_enable` does not check `threads_oneshot` (`chip.c:239-252`) and relies on the flow masking again.
8. **Detach on destroy.**
   - Destroyed is set under L2 (`interrupt_dispatcher.cpp:281-288`).
   - Detach, under L1, unlinks the action, clears held, and masks the line if it was the last action.
   - The object's own mask in destroy (`:290`) becomes a no-op, because `mask_interrupt()` is empty.
   - Linux shuts the line down when the last action goes (`manage.c:1892-1902`). It waits for the thread to finish rather than clearing the bit (`:1278-1283`, `:1935`). Ours clears held because a user thread may never ack.

### Invariant

The intent: on a fasteoi line whose action is Oneshot, outside a dispatch, the pin is masked while held is set. Held is set from each interrupt the action returns WakeThread for until the finalize that follows the ack clearing that debt, or until detach. Open questions asks what happens to an interrupt that lands between an ack and its finalize.

### Lock order

- **Dispatch:** L1, then L2, then LI or L3, unchanged (`.claude/skills/ours-kernel-design/references/task.md:48-50`). The flow's mask and unmask take LI under L1, as attach already does (`irq_object.hpp:61-63`).
- **Finalize:** L1, then LI. It is never called with L2 held, so there is no L2 to L1 edge.
- **From step 8, mask and unmask:** L1, then LI. No handler calls them on its own line any more.

### API sketch

```cpp
namespace ours::irq {
enum class IrqReturn : u8 { None, Handled, WakeThread }; // WakeThread counts as handled
enum class IrqFlags : u32 { None, Shared = 0_bit, Permanent = 1_bit, Oneshot = 2_bit };
auto attach(VIrqNum, Action &, ustl::Option<LineMode>) -> ktl::Result<TriggerMode>; // selects the flow
auto detach(Action &) -> Status;                  // barrier; clears held; masks after the last action
auto finalize_oneshot(VIrqNum, Action &) -> void; // no object lock held; does nothing if the action left
auto mask(VIrqNum) -> Status;                     // from step 8 under L1
auto unmask(VIrqNum) -> Status;                   // from step 8 under L1; leaves a held line masked
auto handle_irq_generic(VIrqNum) -> bool;         // interrupt context: the flow, EOI included; false if no line
auto handle_irq_for_test(VIrqNum) -> IrqReturn;   // thread context: the flow without the chip's EOI
} // namespace ours::irq
```

If `handle_irq_generic` answers false, arch sends the LAPIC EOI itself. Every vector therefore gets exactly one EOI.

### Responsibility allocation

| Responsibility | Today | After | Reason |
|---|---|---|---|
| Vector to line | Static 32+GSI; `faults.cpp` subtracts the base (`interrupt.cpp:99-108`, `:136`; `faults.cpp:21-25`) | arch and platform, unchanged | One 24-pin IO-APIC fits the platform vectors (`interrupt.cpp:105-108`); allocation comes with MSI |
| irq_domain, number spaces | None; a line's number is its GSI (`irq_dispatcher.cpp:62-65`) | kernel/irq, with MSI (D4) | Until a second space exists, the table is one flat domain |
| Chip operations | `IrqChip`; eoi, ack, startup and affinity never called (`io_apic.cpp:138-157`) | kernel/irq calls them; arch supplies the chips | The flow sends the EOI through the chip |
| Flow per trigger type | EOI hard-coded in arch (`faults.cpp:242-246`); mask in the object | kernel/irq: `handle_fasteoi`, `handle_edge` (D8) | The mask gets one writer |
| Level/edge policy for user objects | Object flags set from attach's answer (`interrupt_event_dispatcher.cpp:90-92`; `interrupt_dispatcher.cpp:41-44`) | kernel/irq, by flow (D1) | The object only reports when its debt is cleared |
| Disable depth, lazy disable | None; mask and unmask are stateless (`irq_object.cpp:209-227`) | Not now (D5) | Nothing nests: the HPET's one pair (`hpet.cpp:320-338`), and the PIT only unmasks once (`kernel/arch/x86/kernel/time/time.cpp:94-102`) |
| Resend of a pending edge | None | With lazy disable, later | Only lazy disable leaves an edge to replay (`chip.c:381-388`; `linux/kernel/irq/resend.c:122-150`) |
| Threaded handling, ONESHOT | None; the object masks (`interrupt_dispatcher.cpp:238-240`, `:263-265`) | Both: kernel/irq keeps the Oneshot flag, held and finalize; the object is the thread | In a microkernel the slow half runs in a user thread |
| NEEDACK, coalescing | Object (`kernel/object/include/ours/object/interrupt_dispatcher.hpp:230-249`) | Object | A user-visible protocol that edge lines need too |
| Port delivery, timestamps | Object (`interrupt_dispatcher.cpp:211-268`) | Object | kernel/irq knows no ports; Zircon places it the same way (`interrupt_dispatcher.cc:205-257`) |
| Shared lines | `IrqFlags::Shared`; every observer runs (`irq_object.cpp:55-63`, `:174-186`) | kernel/irq, for kernel actions; user objects stay exclusive | Decided (`.agents/tasks/microkernel-interrupts/DESIGN.md:295`). UIO refuses shared lines (`linux/drivers/uio/uio_pdrv_genirq.c:144-147`); PCI 2.3 INTx sharing, as VFIO does it (`linux/drivers/vfio/pci/vfio_pci_intrs.c:236-240`, `:298-300`), comes with PCI |
| Spurious detection, statistics | Count of None (`irq_object.cpp:201-205`) | kernel/irq: WakeThread counts as handled; stuck-line disable later | Linux counts only `IRQ_NONE` (`linux/kernel/irq/spurious.c:334-346`) and disables at 99900 of 100000 (`:359-375`); ONESHOT paces user-line storms |
| Affinity | Stub (`kernel/kernel/irq/mod.cpp:61-63`; `io_apic.cpp:155-157`) | Untouched | Non-goal |
| Permanent lines | Lockless dispatch (`irq_object.cpp:192-199`; `irq_object.hpp:37-41`) | kernel/irq; their flow reads nothing L1 guards, and Oneshot is refused | The tick line. Zircon has the same (`zircon/kernel/dev/interrupt/include/dev/interrupt.h:136-141`; `interrupt_manager.rs:90-104`) |
| Detach barrier | L1 held across the handlers (`irq_object.hpp:30-35`) | kernel/irq, L1 kept (D2); detach also clears held and masks after the last action | The objects' destroy order rests on it |
| MSI readiness | None (`irq_dispatcher.cpp:19-23`) | Both, later | The four virtuals keep Zircon's in-device mask open (`msi_interrupt_dispatcher.h:72-74`); the Linux model is an MSI chip marked one-shot safe (`irqdomain.c:133`; `manage.c:1545-1554`) |

### Staging

One line per step; each step is introduced to the user before it starts, and moves to PLAN.md.

1. **Record the exception.** Write "kernel/irq follows Linux's genirq" into `references/task.md`, beside the interrupt-context rule (`:35-50`), once D1 and D2 are decided (`SKILL.md:21-22`, `:54`).
2. **Renames per D6.** Mechanical only.
3. **EOI through the chip.**
   - The IO-APIC chip gets an eoi that calls `apic_issue_eoi()`, sent by the flow after the actions. arch sends the EOI only for a vector with no line.
   - Tests switch to `handle_irq_for_test` (`kernel/commands/irq_test.cpp:29-33`; `kernel/commands/interrupt_line_test.cpp:146-152`).
   - `ioapic_issue_eoi`, the directed EOI, stays as it is for x86-apic item 2. `XAPIC_CHIP`, whose `issue_ack` is already the LAPIC EOI (`local_apic.cpp:111-121`), is not bound while x86-apic item 3 stands (`x86-apic.md:43-59`).
4. **Flows per line, selected per D8.** A line with no action is masked. Lines with an action behave as today.
5. **Test-only mask read-back (D10 (b)).** `interruptline.test` gains the level and edge mask checks, passing on today's objects.
6. **ONESHOT in kernel/irq.**
   - Adds the flag, `WakeThread`, held, `finalize_oneshot`, detach clearing held, and WakeThread counted as handled.
   - Exercised by `irq.test` on the idle line, configured level.
7. **Switch the objects (D1 (b)).**
   - Attach with Oneshot; the handler always returns WakeThread.
   - `mask_interrupt()` is empty; `unmask_interrupt()` finalizes outside L2, and only when the debt is cleared.
   - Remove MaskPostwait, UnmaskPrewait and `set_level_triggered`.
8. **mask and unmask take L1**, and unmask leaves a held line masked.

Deferred, each a task of its own:
- depth, lazy disable and resend (D5);
- stuck-line disabling;
- domain and vector allocation, with MSI;
- affinity;
- kernel-irq repair item 3 (D9).

## Alternatives considered

- **Keep the mask in the object, with flows sending only the EOI (Zircon; decided 2026-10-04).** The mask then has two writers: the flow, for lines with no action, and the object. kernel/irq can never keep line state.
- **Replace the four virtuals with one finalize hook (the earlier draft).** An MSI object that masks inside its device, as Zircon's does (`msi_interrupt_dispatcher.h:72-74`), would need them back.
- **UIO/VFIO style: the object calls a counted `disable_nosync` and `enable`** (`uio_pdrv_genirq.c:60-100`; `vfio_pci_intrs.c:232-240`). The handler would call into its own line while dispatch holds L1 (`irq_object.hpp:30-35`), so this needs D2 (b) first.
- **genirq's INPROGRESS model** (`handle.c:259-266`; `manage.c:50-82`). It rewrites the detach barrier and destroy's ordering argument, with no present need.
- **A `held` callback from finalize into the object under L1, copying Linux's RUNTHREAD re-check.** Linux needs the re-check because it runs the handlers without `desc->lock` (`handle.c:83-122`). Under D2 (a) ours does not, and the callback would add an L1 to L2 edge on a new path.
- **A software Masked bit now.** Until step 7 the object masks from its handler without L1, and acks without L1 (`interrupt_event_dispatcher.cpp:113-127`). A bit read and written on both paths would be check-then-act with no common lock, and could leave a level line unmasked in NeedAck.
- **A per-action oneshot bitmap.** There is no consumer: user lines are exclusive, and no kernel action uses Oneshot.
- **Edge EOI before the actions, as Linux's ack does** (`linux/arch/x86/kernel/apic/vector.c:1020-1024`). It changes today's order and buys nothing while L1 is held.
- **In-kernel interrupt threads** (`manage.c:1244-1286`). No kernel driver needs one, and other interrupt-context producers use a Dpc (`references/task.md:50`).
- **A parallel kernel/irq2.** It routes around the layer instead of fixing it (`SKILL.md:23-25`).
- **irq_domain now.** There is one number space until MSI (`irq_dispatcher.cpp:62-64`).
- **The PM-timer SCI as the level test.** It needs FADT decoding, and QEMU's behaviour is unchecked (`docs/repairs/261005/object.md:16`).

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Who owns a level line's mask | (a) the object, as decided on 2026-10-04 (`microkernel-interrupts/DESIGN.md:294-295`); flows only send the EOI; (b) kernel/irq's fasteoi runs ONESHOT; the handler returns WakeThread; the object keeps its four virtuals, with `mask_interrupt()` empty and `unmask_interrupt()` = `finalize_oneshot`, called outside L2 only when the debt is cleared, as Zircon's `UNMASK_PREWAIT_UNLOCKED` unmasks after the lock (`interrupt_dispatcher.h:72-76`); (c) UIO/VFIO's counted disable from the handler, which needs D2 (b) | (b): one writer of the mask; D2's user-visible behaviour and D1's four virtuals are kept; MSI can later follow either model. Record it as a Linux exception | | |
| D2. Whether L1 stays held across the handlers | (a) keep it: Zircon's own model, which runs the handler under the entry lock (`interrupt_manager.rs:105-109`), and detach taking L1 is the barrier (`irq_object.hpp:30-35`); (b) genirq's INPROGRESS flag plus a synchronize spin (`handle.c:259-266`; `manage.c:50-82`) | (a). It is a concurrency decision, so the user writes the argument first. The open L1 to L2 argument (`microkernel-interrupts/DESIGN.md:321-327`) stays the user's, with interleaving B restated for finalize (Open questions) | | |
| D3. In-kernel threaded handlers | (a) none: the object plus its user thread is the thread, and kernel drivers defer with a Dpc; (b) a kthread per action (`manage.c:1257`); (c) a Dpc-based thread_fn adapter | (a) | | |
| D4. irq_domain now or with MSI | (a) a linear IO-APIC domain now; (b) with MSI, keeping `domain.hpp` as scaffolding | (b) | | |
| D5. Disable depth, lazy disable, resend | (a) depth now: attach leaves depth 1 as `IRQF_NO_AUTOEN` does (`manage.c:1763-1776`; `irqdesc.c:131-134`), and `time.cpp:102`, `interrupt_event_dispatcher.cpp:93` and `irq_test.cpp:311-320` move to enable in the same step; Disabled read by the lockless Permanent flow must be atomic, or the tick line takes L1; (b) also lazy disable and resend, where x86 retriggers by IPI to the vector's target CPU (`vector.c:1002-1012`); (c) none now, with idempotent masks as Zircon has (`interrupt_manager.rs:200-210`) | (c): nothing nests (`hpet.cpp:320-338`); depth comes with its second user, stuck-line disable. Pick (a) only for its teaching value | | |
| D6. Naming | (a) genirq role names in the scaffold's short style: `irq::Desc`, `irq::Action`, `irq::Chip`, `irq::Data`, `irq::Domain`, `WakeThread`, `handle_edge`, `handle_fasteoi`; (b) keep today's names; (c) only fill `Action` and rename `IrqDispatcher` | (a): one-to-one with what the user reads in Linux, and `irq::IrqDispatcher` stops colliding with `object::*Dispatcher` (`microkernel-interrupts/DESIGN.md:21`). Comments still do not name Linux | | |
| D7. How the rewrite is staged | (a) in place, one mechanism per step behind `mod.hpp`; (b) a parallel kernel/irq2; (c) one big change | (a) | | |
| D8. Where a line's flow is chosen | (a) at `init_lines` from the mode the platform programmed, re-selected under L1 by `configure()` and attach. The table is built after the pins are programmed (`interrupt.cpp:136-138`, `:152`), and later changes go only through those two; (b) ask the chip on every interrupt, an IO-APIC read under LI each time. This replaces the reason in `irq_object.cpp:251-254` | (a) | | |
| D9. kernel-irq repair item 3 (thread-context dispatch with preemption on) | (a) `notify()` takes L1 with `IrqSaveAndPreemptDisable`, the repair's recommendation (`docs/repairs/261004/kernel-irq.md:55`); (b) leave it; (c) `handle_irq_for_test` turns preemption off, and `handle_irq_generic` asserts interrupt context through `CURRENT_FRAME` (`mod.cpp:80-83`). No cost on the real path | (c), decided on its own; it is not a prerequisite, since `fire()` already turns preemption off (`interrupt_line_test.cpp:146-152`) | | |
| D10. How the level path is verified | (a) the PM-timer SCI on GSI 9, as `object.md` item 1 recommends (`object.md:16`); (b) a test-only read of the pin's mask bit through the chip, read like `trigger_mode` reads the entry (`io_apic.cpp:122-136`), on the free line in ModeLevelHigh | (b): no FADT dependency, and it checks the real register, before the switch and after it | | |

## Testing and verification

- **Every step:**
  - Build and boot both images: `python3 .agents/skills/ours-verify/scripts/boot_check.py`.
  - Then run the kernel tests on dbg, which boot_check does not run: `python3 scripts/kshell_run.py --out <dir> irq.test interruptline.test` (`.agents/skills/ours-verify/SKILL.md:38-56`).
  - Then commit.
- **`irq.test`:**
  - flow selection;
  - a no-action line gets masked;
  - Oneshot sets held, and finalize clears it;
  - detach clears held;
  - Permanent with Oneshot is refused;
  - WakeThread is not counted as spurious.
- **`interruptline.test`:** a level object's pin is masked from delivery to its ack or wait-as-ack; an edge object's pin never is. This passes on today's objects at step 5, and again after step 7.
- **End to end:** the pc-ps2 loop (`system/driver/pc-ps2/pc_ps2.cpp:207`, `:246`, `:255`). The rel boot types keys and requires `pc-ps2: key` lines (`.agents/skills/ours-verify/SKILL.md:24-29`). The tick shows on both images, since boot reaches the shell.
- **Not available:**
  - lockdep for L1 and LI (`docs/repairs/261004/kernel-irq.md:23-26`). Lock order rests on the written argument.
  - A real level device (`docs/repairs/261005/object.md:12`).

## Open questions

- **The finalize exercise, the user's.**
  - What there is: held under L1, the object's state under L2, and a flow that masks every Oneshot interrupt before running its handler.
  - The actors: CPU0 dispatching; T1 acking; T2 destroying; and a test thread raising the line by hand (`interrupt_line_test.cpp:154-160`).
  - The interleaving: T1 moves to Idle and drops L2; before its finalize takes L1, an interrupt is dispatched.
  - The questions: Can a real level interrupt land there? What does a hand-raised one do? Why does ours need neither Linux's INPROGRESS spin nor its RUNTHREAD re-check (`manage.c:1110-1123`)? What would change under D2 (b)?
- **Interleaving B, restated** (`.agents/tasks/microkernel-interrupts/DESIGN.md:326`).
  - Under D1 (b): T1 sets Waiting and drops L2, finalizes, then parks; an interrupt lands between the finalize and the park.
  - Finalize runs before `BEFORE_PARK_HOOK` (`interrupt_dispatcher.cpp:53-61`), so interrupt.test can still play this interleaving.
  - It belongs to the user's open L1 to L2 argument (`:321-327`).
- **MSI masking.** When MSI comes, does it get a chip marked one-shot safe, as in Linux, or an object that masks inside the device, as in Zircon?
- **An edge on a masked pin.** That the IO-APIC drops it is stated without a source in the earlier design (`.agents/tasks/microkernel-interrupts/DESIGN.md:279`) and is not checked here. It matters only if D5 (b) is chosen.
- **Remote IRR.** Whether the LAPIC EOI clears a level pin's Remote IRR on ours's setup is unverified, since no level line has fired yet (`microkernel-interrupts/DESIGN.md:316`; `x86-apic.md:9`).
- **x86-apic item 2.** It stays off the path while the chip's eoi is the LAPIC EOI. Confirm this when step 3 lands.
