# kernel-irq changes

## Title: kernel/irq is to follow genirq, recorded as a Linux exception, and the irq-genirq design accepted (2026-10-05)

**Files**
- `.agents/tasks/irq-genirq/DESIGN.md` — Status Accepted; every Decided cell holds the recommended option with "2026-10-05, the user took the recommendation and skipped the prediction"; the prediction cells say Skipped for D1 and D2, the two the user was asked about, and Not asked for the rest
- `.agents/skills/ours-kernel-design/references/task.md` — new section "Interrupt lines: kernel/irq follows genirq", beside the interrupt-context rule: what is modelled on genirq, the reason, and what is not taken from it
- `.agents/skills/ours-kernel-design/SKILL.md` — the "Where to look" row for `references/task.md` names interrupt lines, so a session working on kernel/irq finds the rule
- `.agents/tasks/irq-genirq/PLAN.md` (new) — the steps as they start, as the DESIGN's Staging says, with step 1 and what every step runs
- `docs/changes/261005/kernel-irq.md` (new) — this record

**Symptom**
None, no code changes: step 1 of the DESIGN's Staging. Without it, a session that loads `ours-kernel-design` before touching kernel/irq reads "Zircon is the default spec" and finds no exception for interrupt lines. The later steps (flows in kernel/irq, the EOI sent by the flow, the object's `mask_interrupt()` left empty) would then read as drift from Zircon, to be "fixed" back.

**Cause**
The objects:
- L1, the line's spin lock (`IrqObject::m_lock` today). `notify()` holds it while the handlers run, so `detach()`, which takes it too, is the barrier the interrupt objects' destroy order rests on (`kernel/include/ours/irq/irq_object.hpp:30-35`).
- L2, the interrupt object's lock; LI, the IO-APIC's lock, under which every pin write is a read-modify-write.
- The mask bit in the pin's IO-APIC redirection entry: the only record of whether a line is masked.
- The interrupt object (`InterruptEventDispatcher`): delivers to its port or its waiter and, on a level line, keeps the pin masked from delivery to the ack (D2 of 2026-10-04 in `.agents/tasks/microkernel-interrupts/DESIGN.md`).

Two coherent models of who writes that mask bit:
- In Zircon it is a property of the object kind. The event object sets MaskPostwait and UnmaskPrewait for a level line; the layer below runs one fixed path for every vector, the handler and then the EOI. Whoever owns the object writes the mask.
- In Linux it is a property of the line's flow. `handle_fasteoi_irq()` masks a ONESHOT line before its handlers, and `irq_finalize_oneshot()` unmasks it once the threaded handler is done. The threaded handler never touches the controller.

Why ours moves to the second:
- **Two writers with no common lock.** Today the object writes the pin from its handler (MaskPostwait) and from its ack (UnmaskPrewait). `irq::mask()` and `irq::unmask()` take no L1, because the handler that masks runs under L1 already and would spin on its own dispatch's lock (`irq_object.hpp:78-82`). The HPET masks the PIT's line the same way. So the mask is written from interrupt context under L1 and from a syscall without it.
- **So kernel/irq can keep no line state.** A held bit, or a software Masked bit, written on both paths would be a check-then-act with no lock both sides take. One interleaving leaves a level line unmasked while its object is still in NeedAck: the device's line is still asserted, the EOI goes out, and the line fires again at once, an interrupt storm the object can only coalesce.
- **A line with no action cannot be silenced.** It is only counted as spurious (`kernel/kernel/irq/irq_object.cpp:201-205`) and EOIed; a level line in that state would fire forever. A flow that owns the mask masks it.
- **The EOI is hard-coded in arch** after the handler, with a TODO to move it into the chip (`kernel/arch/x86/kernel/faults.cpp:242-246`), and the chip's eoi, ack, startup and affinity are never called. With flows in kernel/irq each vector gets exactly one EOI, through the chip.
- **Nothing user space sees changes.** A level line still stays masked from delivery to the ack, and an edge line is still only coalesced. The object keeps its four virtuals, so an MSI object that masks inside its device, as Zircon's does, still fits later.

Why L1 stays held across the handlers (D2 (a)):
- Linux drops `desc->lock` while the handlers run and marks the line INPROGRESS instead. `synchronize_irq()` spins on that flag to be the barrier, and `irq_finalize_oneshot()` spins on it too, since the thread can be done before the hard handler on another CPU has left.
- Ours keeps the handlers under L1, as Zircon runs its handler under the per-vector entry lock, so detach taking L1 is already the barrier. Taking genirq's INPROGRESS model would rewrite the destroy ordering argument with no present need. What L1 held saves the finalize (Linux's INPROGRESS spin and RUNTHREAD re-check) is left as the user's exercise in the DESIGN's Open questions.

Why the rule goes into the skill and not only into the DESIGN:
- `SKILL.md` states that Zircon is the default spec and that the Linux exceptions are listed in the subsystem files. A DESIGN under `.agents/tasks/` is not read by a session that loads only the skill, so the exception has to sit next to the default it overrides, or the next reviewer reverts it as a deviation.

**Change**
The DESIGN, before and after (D1's row shortened):

```text
**Status**: Proposed
| D1. Who owns a level line's mask | ... | (b): one writer of the mask; ... Record it as a Linux exception | | |
```

```text
**Status**: Accepted
| D1. Who owns a level line's mask | ... | (b): ... | Skipped: the user said "直接做" (2026-10-05) | (b), as recommended: kernel/irq's fasteoi flow owns a level line's ONESHOT mask, and the object is the threaded handler and keeps its four virtuals. Recorded as a Linux exception in `.agents/skills/ours-kernel-design/references/task.md`. 2026-10-05, the user took the recommendation and skipped the prediction |
```

`references/task.md` gains a section after "Posting a port from interrupt context", wrapped at 80 columns because skills are read raw. Its core:

```text
## Interrupt lines: kernel/irq follows genirq

`kernel/irq` is modelled on Linux's genirq, not on Zircon's interrupt
manager: a deliberate Linux exception, the user's call of 2026-10-05 (D1 (b),
D2 (a) and D6 (a) in `.agents/tasks/irq-genirq/DESIGN.md`, which names each
counterpart). The code reaches it step by step (the DESIGN's Staging); until
step 7 the interrupt objects still write the level mask themselves.
```

followed by one bullet each for the line descriptor, chip and data; the per-line flows; ONESHOT owned by the line; the interrupt object as the threaded handler, with why its finalize waits until the debt is cleared; L1 held across the handlers; the Permanent line as the one dispatched without L1; and ours's own rules inside the genirq model (detach clears held, unmask leaves a held line masked from step 8, no software Masked bit). Then the reason and the list of what is not taken from genirq.

The Permanent bullet and the own-rules bullet came from review: without them the section stated "L1 stays held across the handlers" with no exception, which a session building the flows could have applied to the tick line, and the places where ours departs from genirq inside kernel/irq were recorded only in the DESIGN, where a session that loads only the skill would "fix" them back to genirq.

Rejected: putting the rule into the existing interrupt-context section. That section is about one exception to the Dpc rule (posting a port from the handler); folding a second, larger exception into it would hide both.

**Reference**
What kernel/irq follows, piece by piece:
- Line descriptor: Linux `struct irq_desc` (`include/linux/irqdesc.h`), found by `irq_to_desc()` (`kernel/irq/irqdesc.c`).
- Action: Linux `struct irqaction` (`include/linux/interrupt.h`), linked by `__setup_irq()` (`kernel/irq/manage.c`).
- Chip and data: Linux `struct irq_chip` and `struct irq_data` (`include/linux/irq.h`).
- Level flow: Linux `handle_fasteoi_irq()`, with `mask_irq()` and `cond_unmask_eoi_irq()` for a ONESHOT line (`kernel/irq/chip.c`).
- Edge flow: Linux `handle_edge_irq()` (`kernel/irq/chip.c`).
- Flow chosen from the trigger mode: Linux `mp_register_handler()` (`arch/x86/kernel/apic/io_apic.c`).
- The interrupt object as the threaded handler, its ack as the finalize: Linux `irq_thread()`, `irq_thread_fn()` and `irq_finalize_oneshot()` (`kernel/irq/manage.c`).

What is kept from Zircon:
- L1 held across the handlers: Zircon's interrupt manager invokes the handler under its per-vector entry lock (`zircon/kernel/platform/pc/interrupt_manager.rs:105-109`).
- Unmasking after the object's lock is dropped: Zircon `InterruptDispatcher::AckInternal()` with `INTERRUPT_UNMASK_PREWAIT_UNLOCKED` (`zircon/kernel/object/interrupt_dispatcher.cc`, `zircon/kernel/object/include/object/interrupt_dispatcher.h`). Only in part: `AckInternal()` defers the unmask on every ack in NEEDACK, the retriggered one too, so `InterruptDispatcher::Bind()` refuses a port for an object that has both `INTERRUPT_UNMASK_PREWAIT_UNLOCKED` and `INTERRUPT_MASK_POSTWAIT` (`interrupt_dispatcher.cc:312-318`): the mask the next packet makes under the lock would be undone by the deferred unmask. Under D1 (b) a bound level object in ours is the same combination, the flow's mask at dispatch and an unmask after L2, which is why its finalize runs only on the ack that moves it to Idle and never on one that sends the next packet.
- The four virtuals and the delivery itself: Zircon `InterruptDispatcher::InterruptHandler()`.

Not followed: Linux `handle_irq_event()`'s INPROGRESS flag and `synchronize_irq()` (`kernel/irq/handle.c`, `kernel/irq/manage.c`), `struct irq_domain` until MSI, disable depth (`irq_disable()`, `irq_enable()`) and `check_irq_resend()`.

**Verification**
`python3 scripts/lint_docs.py --changed` on the documents and this record: no violations. Every non-table line of `references/task.md` fits in 80 columns (checked with awk). Docs only, so the step itself ran no build and no boot. In review, as the baseline for step 2: one `boot_check.py` run built `out/bringup.x86` (32.3 s) and passed both images (dbg 34 passed, 2 skipped; rel 30 passed, 10 skipped, 6/6 required, with `pc-ps2: key a` and `pc-ps2: key A`), and one `kshell_run.py` run on dbg gave `irq.test: ok -- 33 checks`, `interruptline.test: ok -- 41 checks` and `interrupt.test: ok -- 32 checks`.

**Deliberately not done**
- The DESIGN stays in `.agents/tasks/irq-genirq/`, not moved to `docs/design/`: its Staging is being carried out step by step and will gain Revised notes, as `.agents/tasks/microkernel-interrupts/DESIGN.md` stayed while Accepted.
- PLAN.md lists only step 1: the Staging says each step moves there when it starts, so the later steps are not written out ahead of time.
- The Open questions (the finalize exercise, interleaving B under D1 (b)) stay open: they are the user's to answer, and taking the recommendation did not answer them.
- No code: steps 2 to 8 each land as their own commit.

**How it was found**
None.

**The user's prediction**
None made. The user took every recommendation and skipped both the prediction for D1 and the concurrency argument for D2 ("直接做", 2026-10-05).

**Three questions you should be able to answer**
1. Why does writing the level mask from both the object's handler (under L1) and its ack (without L1) stop kernel/irq from keeping a held bit, and what does the worst interleaving leave the line in?
2. What makes detach a barrier while L1 stays held across the handlers, and what would have to take its place if the lock were dropped while they run?
3. What does user space observe differently on a level line once the mask moves from the object into the fasteoi flow, and why is the answer "nothing"?

## Title: kernel/irq's types are named after their genirq roles: irq::Desc, irq::Action, irq::Chip, irq::Data and irq::DescTable (2026-10-05)

**Files**
- `kernel/include/ours/irq/desc.hpp` (was `irq_object.hpp`), `kernel/kernel/irq/desc.cpp` (was `irq_object.cpp`) — `IrqObject` is `irq::Desc`; its `m_observers` and `permanent_observers()` are `m_actions` and `permanent_actions()`
- `kernel/include/ours/irq/action.hpp` — the empty `class Action {}` placeholder is filled with what was `IrqObserver`; `irq_observer.hpp` is gone
- `kernel/include/ours/irq/chip.hpp` (was `irq_chip.hpp`) — `IrqChip` and `IrqData` are `irq::Chip` and `irq::Data`
- `kernel/include/ours/irq/desc_table.hpp` (was `irq_dispatcher.hpp`), `kernel/kernel/irq/desc_table.cpp` (was `irq_dispatcher.cpp`) — `irq::IrqDispatcher` is `irq::DescTable`; `objects_` is `m_descs`, `irqnum_to_object()` is `irqnum_to_desc()`
- `kernel/kernel/irq/mod.cpp` — `get_irq_dispatcher()` and its static `IRQ_DISPATCHER` are `get_desc_table()` and `table`
- `kernel/include/ours/irq/mod.hpp`, `kernel/include/ours/irq/types.hpp` — the declarations and their comments say action
- `kernel/kernel/irq/BUILD.gn` — the two renamed sources, in the same places in the list
- `kernel/object/interrupt_event_dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp` — the object's `irq::IrqObserver m_observer` is `irq::Action m_action`
- `kernel/object/interrupt_dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_dispatcher.hpp` — comments only
- `kernel/arch/x86/apic/io_apic.cpp`, `kernel/arch/x86/apic/local_apic.cpp`, `kernel/arch/x86/kernel/include/ours/arch/apic.hpp` — the chips are `irq::Chip` and their operations take `irq::Data`
- `kernel/arch/x86/kernel/time/time.cpp` — the PIT's `LEGACY_TIMER_OBSERVER` is `LEGACY_TIMER_ACTION`
- `kernel/arch/x86/kernel/faults.cpp`, `kernel/arch/x86/kernel/irq/init.cpp` — a TODO and an include
- `kernel/commands/irq_test.cpp` — the new names, in the code, the comments and the check names
- `docs/repairs/261004/kernel-irq.md`, `docs/repairs/261003/x86-apic.md` — the open items name `irq::Desc::m_lock`, `irq::Desc::notify()`, `desc.cpp` and `irq::Data`
- `.agents/tasks/irq-genirq/PLAN.md` — step 2

**Symptom**
None, a rename with no behaviour change: step 2 of the DESIGN's Staging (D6 (a)). The images boot and the kernel tests pass with the same check counts as before.

**Cause**
The objects, under their old names and what each really is:
- `IrqObject`: one per interrupt line. It holds L1, the line's flags, the line's `IrqData` and the list of handlers. Steps 4 and 6 add the flow, the Oneshot flag and the held bit to it.
- `IrqObserver`: one handler on one line, owned by whoever registered it. It is called on every interrupt, in interrupt context, and answers whether it handled it. Step 6 adds the Oneshot flag to it.
- `IrqChip` and `IrqData`: the controller's operations, and the line as the controller sees it (hwirq, virq, chip). Steps 3 and 5 add the eoi and the mask read-back.
- `irq::IrqDispatcher`: a flat array from a line number to its line, built once by the platform and read without a lock.

Why those names misled, each in this kernel's own vocabulary:
- **"Dispatcher" is a kernel object here.** `object::Dispatcher` and its family (`InterruptDispatcher`, `PortDispatcher`, ...) are what a handle refers to: refcounted, created by a syscall, destroyed with their last handle. The line table is none of that. `interrupt_event_dispatcher.cpp` had two "dispatchers" one layer apart, and the earlier design already flagged the collision (`.agents/tasks/microkernel-interrupts/DESIGN.md`, Today's objects).
- **"Observer" is a signal watcher here.** `object::SignalObserver` (`kernel/object/include/ours/object/observer.hpp`) is armed on a dispatcher, is told once when a set of signals matches, and a port counts its armed observers against `PORT_MAX_OBSERVERS`. An irq handler is not armed and not told once: it runs on every interrupt and decides whether the interrupt was its device's. The interrupt object held an `m_observer` of the first kind beside a port full of the second.
- **"Object" is a kernel object here** too, and the line is not one: no handle names it and user space never sees it. It is the line's descriptor.

Why genirq's names and not some other new ones (D6 (a)):
- Each later step is compared with its genirq counterpart, so the names map one to one: `irq::Desc` is `struct irq_desc`, `irq::Action` is `struct irqaction`, `irq::Chip` and `irq::Data` are `struct irq_chip` and `struct irq_data`, and `irq::DescTable` is what `irq_to_desc()` indexes.
- The short style is the scaffold's own: `action.hpp` already declared `irq::Action`, and the `irq::` namespace already says what kind of descriptor or chip it is, so an `Irq` prefix repeats it.
- Code comments still say what each thing is in this kernel's terms and never name the system the names come from. The mapping lives in the DESIGN and in this record's Reference.

Why it is a step of its own, and first:
- Every later step adds to these types. Renamed inside step 3 or 4, each of those diffs would carry hundreds of lines of name churn around the few lines of mechanism the user reviews.
- The layer is small now: twenty C++ files and one GN file, every caller in the tree included. It only grows from here.

How the files follow the types:
- A file is named for what it holds: `desc.hpp` holds `Desc`, `desc_table.hpp` holds `DescTable`.
- `Chip` and `Data` share `chip.hpp`, because `Data` is what every chip operation is handed and nothing uses one without the other.
- The `action.hpp` placeholder is filled rather than replaced: the empty `class Action {}` was scaffolding for exactly this role (DESIGN, Problem), so `IrqObserver`'s body moved into it, keeping its include guard and header, and `irq_observer.hpp` was removed.

**Change**
The renames:

```text
IrqObject            -> irq::Desc        irq_object.hpp/.cpp     -> desc.hpp/.cpp
IrqObserver          -> irq::Action      irq_observer.hpp        -> action.hpp (fills the placeholder)
IrqChip, IrqData     -> irq::Chip, Data  irq_chip.hpp            -> chip.hpp
irq::IrqDispatcher   -> irq::DescTable   irq_dispatcher.hpp/.cpp -> desc_table.hpp/.cpp
```

What a caller sees, before and after:

```cpp
// before
irq::IrqObserver m_observer;
constinit irq::IrqObserver LEGACY_TIMER_OBSERVER{&timer_interrupt, &LEGACY_TIMER,
                                                 irq::IrqFlags::Permanent, "IRQ-Timer"};
auto io_apic_chip() -> irq::IrqChip const &;
```

```cpp
// after
irq::Action m_action;
constinit irq::Action LEGACY_TIMER_ACTION{&timer_interrupt, &LEGACY_TIMER,
                                          irq::IrqFlags::Permanent, "IRQ-Timer"};
auto io_apic_chip() -> irq::Chip const &;
```

Inside kernel/irq, every parameter, local and comment that said observer says action, and the table's own names follow its type:

```cpp
// before
static IrqDispatcher IRQ_DISPATCHER;
ustl::views::Span<IrqObject *> objects_;
```

```cpp
// after
static DescTable table;
ustl::views::Span<Desc *> m_descs;
```

The last two also settle clang-tidy's `readability-identifier-naming` on lines this step rewrote: a private member takes the `m_` prefix, and a function-local static is a local variable, lower case. The lines' old names drew the same two warnings. The table's line name, an unused debug string, went from `"IRQO"` to `"IRQD"`.

In the arch files, which `using namespace irq`, the chip types are written `irq::Chip` and `irq::Data`. Unqualified, `Data` would read as anything.

Rejected:
- D6 (c), renaming only `IrqObserver` and `IrqDispatcher`: it leaves `IrqObject` named as a kernel object, and step 4's flows would then go into a type named for the wrong layer.
- Removing `action.hpp` and moving `irq_observer.hpp` onto its path: the same file in the end, but history would show the scaffold deleted rather than filled.
- `Data` in a `data.hpp` of its own: nothing includes one without the other.

**Reference**
Each name, and the genirq piece it follows:
- `irq::Desc`: Linux `struct irq_desc` (`include/linux/irqdesc.h`).
- `irq::Action`: Linux `struct irqaction` (`include/linux/interrupt.h`), linked onto a line by `__setup_irq()` and taken off by `__free_irq()` (`kernel/irq/manage.c`).
- `irq::Chip` and `irq::Data`: Linux `struct irq_chip` and `struct irq_data` (`include/linux/irq.h`).
- `irq::DescTable`: the table behind Linux `irq_to_desc()`, the `irq_desc[]` array when `CONFIG_SPARSE_IRQ` is off (`kernel/irq/irqdesc.c`).

The Zircon counterparts the old names stood beside: `InterruptTableEntry` for the line and `interrupt_handler_t` for the handler (`zircon/kernel/platform/pc/interrupt_manager.rs`, `zircon/kernel/dev/interrupt/include/dev/interrupt.h`). Zircon has no separate chip or data type.

**Verification**
- Formatting: `scripts/format_cpp.py` on all twenty C++ files, which reformatted only lines this step changed, and `scripts/format_gn.py` on `kernel/kernel/irq/BUILD.gn` (already in layout).
- clang-tidy, with `-p out/bringup.x86`, on the eleven touched sources (the six headers through them). Three diagnostics fell on changed lines, all `readability-identifier-naming`, and all drawn by the old names too. `descs_` and `DESC_TABLE` were fixed as above. `XAPIC_CHIP` in `local_apic.cpp` was left: only its type changed on that line, and the name is UPPER_CASE like every other global in the file.
- `git grep` for `IrqObject`, `IrqObserver`, `IrqChip`, `IrqData`, `IrqDispatcher` and the four old file names finds nothing outside the design documents and older change records.
- `boot_check.py`, run twice (after the rename, then after the two naming fixes): build ok, `dbg PASS` (34 passed, 2 skipped, 0 failed), `rel PASS` (30 passed, 10 skipped, 6/6 required), with `pc-ps2: key a` and `pc-ps2: key A` in `rel.log` both times.
- `kshell_run.py --out out/bringup.x86 irq.test interruptline.test interrupt.test`, run twice: `irq.test: ok -- 33 checks`, `interruptline.test: ok -- 41 checks`, `interrupt.test: ok -- 32 checks`, the same counts as the baseline run for step 1.
- Load: two boot_check runs (four boots) and two kshell runs; no repeated-boot stress.

**Deliberately not done**
- `notify()`, `on_event()`, `IrqReturn`, `IrqFlags` and `IrqHandler` keep their names. The DESIGN's API sketch keeps `IrqReturn` and `IrqFlags`, and `notify()` gives way to the flows in steps 3 and 4, so renaming it now would rename it twice.
- `WakeThread`, `handle_edge` and `handle_fasteoi` arrive with their mechanisms, in steps 4 and 6.
- `domain.hpp` stays empty: D4 brings the domain with MSI.
- The DESIGN, the older design documents and older change records keep the old names: they are history. The one forward reference, Events step 4's `finalize_oneshot(m_line, m_observer)`, is noted in PLAN.md's step 2 as `m_action`.
- `kernel/arch/x86/kernel/irq/init.cpp` still includes the table's header though it uses nothing from it. Its include was renamed, not removed: dropping it is cleanup outside a rename.

**How it was found**
None.

**The user's prediction**
None made. The user took the recommendation for D6 and skipped the prediction ("直接做", 2026-10-05).

**Three questions you should be able to answer**
1. In this kernel, what makes something a "dispatcher", and which of those properties does the line table lack?
2. An `object::SignalObserver` and an `irq::Action` are both called when something happens. How do they differ in when they are called, in what context, and in what their answer means?
3. Why do the renames land as a step before the flows rather than inside the step that adds them?
