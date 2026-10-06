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
- `docs/repairs/261004/x86-clock.md` — item 2's rejected option names the PIT line's action, not its observer (added in review)
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
- In review, three checks a rename needs beyond the build:
  - No logic changed. The parent's fifteen files with code in them were put through the renames by `sed`, then diffed against this commit with whitespace ignored. What is left is formatting, the locals `object` and `objects` now `desc` and `descs`, the include guards, the debug string, the placeholder's license header, and comments: "observer" now reads "action", and "line object" now reads "line descriptor". `notify()`, `attach()`, `detach()` and `link_locked()` are otherwise token for token the same. So the lock order L1 -> L2 -> {LI, L3}, the detach barrier and the Permanent line's lockless dispatch stand as before.
  - Nothing stale satisfied the link. An `out/` built before a rename keeps the old objects, which can hide a broken build, so the step was rebuilt into a fresh `out/review-step2`. Build ok (33.1 s), `dbg PASS` (34 passed, 2 skipped, 0 failed), `rel PASS` (30 passed, 10 skipped, 6/6 required), with `pc-ps2: key a` and `pc-ps2: key A`. `kshell_run.py --out out/review-step2` gave `irq.test: ok -- 33 checks`, `interruptline.test: ok -- 41 checks` and `interrupt.test: ok -- 32 checks`.
  - Old vocabulary in open repair items. A grep found one irq "observer" left in an open item (`x86-clock.md` item 2), now "action". The other "observer" hits in `docs/repairs` are port and job observers, which keep the name.
  - Review load: one boot_check run (two boots) and one kshell run.

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

## Title: each interrupt is ended by its line's flow through the line's chip, arch ends only a vector with no line, and the tests get the flow without the end of interrupt (2026-10-05)

**Files**
- `kernel/include/ours/irq/types.hpp` — new `irq::Eoi { Send, Skip }`: whether a dispatch ends the interrupt at the line's controller
- `kernel/include/ours/irq/desc.hpp`, `kernel/kernel/irq/desc.cpp` — `Desc::notify()` is `Desc::handle_irq(Eoi)`, the line's flow: the actions, then `end_irq()`, the chip's `issue_eoi`, inside the same hold of L1 (without L1 on a Permanent line)
- `kernel/include/ours/irq/desc_table.hpp`, `kernel/kernel/irq/desc_table.cpp` — `DescTable::handle_irq()` takes the `Eoi` and answers none for a number with no line; `init()` refuses a chip with no `issue_eoi`
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp` — `handle_irq_generic()` answers `bool`, whether a line took (and so ended) the interrupt; new `handle_irq_for_test()`, the flow with `Eoi::Skip`; `init_lines()` documents the new refusal
- `kernel/include/ours/irq/chip.hpp` — `issue_eoi` says who calls it, when, and under which lock
- `kernel/arch/x86/apic/io_apic.cpp` — new `ioapic_eoi()`, the local APIC's EOI, bound as the chip's `issue_eoi`; `ioapic_issue_eoi()`, the directed EOI, unchanged and reached only through `issue_ack`
- `kernel/platform/include/ours/platform/interrupt.hpp`, `kernel/platform/pc/interrupt.cpp` — `platform_handle_irq()` passes the answer up
- `kernel/arch/x86/kernel/faults.cpp` — the platform vectors' hard-coded `apic_issue_eoi()` and its TODO are gone; arch sends it only when no line took the vector
- `kernel/commands/irq_test.cpp` — every dispatch goes through `handle_irq_for_test()`; six new checks on lines of the test's own bound to a chip that only counts its ends of interrupt
- `kernel/commands/interrupt_line_test.cpp` — `fire()` goes through `handle_irq_for_test()`; two checks that arch ends a vector no line has (added in review)
- `kernel/arch/x86/kernel/faults.cpp`, `kernel/include/ours/irq/mod.hpp` — in review: the comment no longer counts every IPI to a platform vector as one with no line, and `init_lines()` says an interrupt before the table is ended by its caller rather than "finds nothing to notify"
- `docs/repairs/261003/x86-apic.md` — items 1 and 2 say why the directed EOI is off the path now; item 3 says binding `XAPIC_CHIP` moves its EOI from `issue_ack` to `issue_eoi` (added in review)
- `docs/repairs/261004/kernel-irq.md` — item 3 names `handle_irq()` and `handle_irq_for_test()`, and records D9 (c) as its decided change
- `.agents/tasks/irq-genirq/PLAN.md` — step 3; `.agents/tasks/irq-genirq/DESIGN.md` — the open question on x86-apic item 2 answered

**Symptom**
None observable: step 3 of the DESIGN's Staging, and each vector still gets exactly one EOI. Before it:
- The EOI of every platform vector was hard-coded in arch after the handler, with `// TODO(SmallHuaZi) Replace it with irq::Chip::issue_eoi()` (`faults.cpp`).
- The IO-APIC chip's `issue_eoi` was the directed EOI, which writes the GSI where the vector belongs (x86-apic item 2), and nothing called it.
- `irq.test` and `interruptline.test` drove `irq::handle_irq_generic()`, the entry a real interrupt reaches, from thread context. That was harmless only because the EOI was not in it. Moving the EOI into the flow without a second entry would have sent more than 166000 local APIC EOIs from a thread on every `irq.test` run, almost all from its barrier check.

**Cause**
The objects:
- The local APIC's in-service register (ISR): one bit per vector. Delivering a vector to the CPU sets its bit; an EOI written to the local APIC clears the highest set bit.
- The processor priority: while a vector's ISR bit is set, the local APIC holds back, on that CPU, every interrupt whose priority class (vector / 16) is not higher than that vector's. The interrupts wait in IRR.
- The IO-APIC redirection entry's Remote IRR, for a level-triggered pin: set when the IO-APIC delivers the pin's interrupt, cleared when an EOI for the entry's vector reaches the IO-APIC. While it is set the pin delivers nothing more. ours leaves the local APIC's EOI broadcast on (the Suppress EOI Broadcast bit is never set), so a local APIC EOI for a level-triggered vector is also sent on to the IO-APICs.
- The line's flow (`Desc::handle_irq()`, `notify()` before this step): runs the line's actions under L1, its lock, or without L1 on a Permanent line.
- The line's chip (`irq::Chip`): the controller's operations, read without a lock because they are fixed before the line is published.

Why exactly one end of interrupt per interrupt, and not "at least one":
- **Zero** is not late, it is never. The vector's ISR bit stays set, so every interrupt of its class and below waits on that CPU for an EOI nobody will send: on today's platform, where every pin targets the boot CPU, the keyboard and every other device line in that class go silent for good. A level pin also keeps Remote IRR set and never delivers again.
- **Two** is the same mistake the other way. The second EOI clears whichever bit is highest in the ISR at that moment. ours does not nest interrupts today, so there is none and it does nothing. With nesting it would end an interrupt that is still being handled, and let another of its class in under that handler. With a chip whose end of interrupt reaches the IO-APIC itself (the directed EOI), it would clear a level pin's Remote IRR early, and the pin would deliver again before its device was serviced.

Why the end of interrupt belongs to the line's flow and not to arch:
- arch knows only the vector. The order of mask, actions, unmask and end of interrupt is exactly what tells the level flow from the edge one in steps 4 and 6. `handle_fasteoi` masks a Oneshot line, runs the actions, leaves the line masked while the debt is held, and only then ends the interrupt; a line with no action is masked and then ended. A flow that cannot place the end of interrupt cannot express either.
- What the end of interrupt is depends on the line's controller, and only the line knows its controller. On the IO-APIC today it is the local APIC's EOI. A chip that ends the pin directly (x86-apic item 2's fix), an MSI chip, or another architecture's controller whose end of interrupt names the interrupt it ends, needs the line's data, which arch never sees.
- One sender per vector, as step 7 gives the mask one writer. A vector that reaches a line is ended by the line's flow, and one that reaches no line is ended by arch. `handle_irq_generic()` answering `bool` is the handshake between the two: true means a line took the interrupt and its flow has ended it.

Why `init_lines()` refuses a chip with no `issue_eoi`:
- The handshake only holds if "a line took it" implies "it was ended". A line bound to a chip with no end of interrupt would answer true, arch would send nothing, and the vector would stay in service for good. The table checks this once, when the lines are made, rather than the flow checking it on every interrupt. The flow keeps a `DEBUG_ASSERT` for a line outside the table dispatched with Send.

Why the tests get an entry of their own:
- A test plays an interrupt from thread context. No interrupt is in service on that CPU, so there is nothing to end. A local APIC EOI from a thread has no in-service bit to clear today, and is harmless only by luck of the chip: an end of interrupt that reaches the line's controller itself would tell the controller the line was serviced while a real interrupt from it may still be in service on another CPU.
- So `handle_irq_for_test()` runs the same flow, under the same locks, with `Eoi::Skip`. `irq.test`'s barrier check, which dispatches the idle line in a loop on a second CPU against attach and detach, still tests the real flow and the real L1.

Why the end of interrupt is sent inside L1, already in this step:
- It is the flow's last step in the DESIGN's Events (step 3 there), as it is under the descriptor lock in the flow it follows. Putting it there now makes step 4 a split of this function into two flows, rather than a split plus a move.
- No correctness argument needs it inside or outside. The local APIC EOI is a write to this CPU's own register, needs no lock, and interrupts are off on this CPU either way.
- It lengthens L1's hold by one local APIC register write, not measured. L1 is contended only by attach and detach, and by a dispatch of the same line on another CPU, which today's routing (every pin to the boot CPU) does not produce outside the tests.
- On a Permanent line, the tick's, the end of interrupt goes out without L1, like the actions. It reads only the chip, which is fixed before the line is published.

**Change**
arch, before and after:

```cpp
// before
case IrqVec::PlatformIrqMin... IrqVec::PlatformIrqMax:
    platform_handle_irq(irqvec_to_irqnum(vector), frame);
    // TODO(SmallHuaZi) Replace it with irq::Chip::issue_eoi()
    apic_issue_eoi();
    break;
```

```cpp
// after
case IrqVec::PlatformIrqMin... IrqVec::PlatformIrqMax:
    if (!platform_handle_irq(irqvec_to_irqnum(vector), frame)) {
        apic_issue_eoi();
    }
    break;
```

The line's flow, before and after:

```cpp
// before
auto Desc::notify() -> IrqReturn {
    auto result = IrqReturn::None;
    if (is_permanent()) {
        result = notify_all(permanent_actions());
    } else {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        result = notify_all(m_actions);
    }
    // spurious count ...
}
```

```cpp
// after
auto Desc::handle_irq(Eoi const eoi) -> IrqReturn {
    auto result = IrqReturn::None;
    if (is_permanent()) {
        result = notify_all(permanent_actions());
        end_irq(eoi);
    } else {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        result = notify_all(m_actions);
        end_irq(eoi);
    }
    // spurious count ...
}

auto Desc::end_irq(Eoi const eoi) const -> void {
    if (eoi == Eoi::Skip) {
        return;
    }
    auto const *const chip = m_data.chip;
    DEBUG_ASSERT(chip && chip->issue_eoi, "line {} ends an interrupt with no chip eoi",
                 m_data.virqnum);
    chip->issue_eoi(m_data);
}
```

The two entries:

```cpp
auto handle_irq_generic(VIrqNum virqnum) -> bool;       // interrupt context; false: no line, caller ends it
auto handle_irq_for_test(VIrqNum virqnum) -> IrqReturn; // thread context; the flow with Eoi::Skip
```

The chip, before and after:

```cpp
// before
.issue_eoi = ioapic_issue_eoi,   // directed EOI to the IO-APIC, never called
.issue_ack = ioapic_issue_ack,   // calls ioapic_issue_eoi
```

```cpp
// after
.issue_eoi = ioapic_eoi,         // apic_issue_eoi(): the local APIC EOI
.issue_ack = ioapic_issue_ack,   // still calls ioapic_issue_eoi; nothing calls it
```

`irq.test` gains a chip that drives nothing and only counts the interrupts it is asked to end, `COUNTING_CHIP`, bound to lines of the test's own outside the table, so no controller sees a test's end of interrupt. With it, six new checks:
- a dispatch ends its interrupt through the line's chip once;
- a played dispatch (`Skip`) runs the actions and ends nothing;
- an interrupt no action handled is ended all the same;
- the Permanent line, dispatched without L1, ends its interrupt once;
- the line table refuses a chip with no eoi (`InvArgs`, before anything is allocated);
- binding the counting chip, taking the line and giving it back.

Rejected:
- The end of interrupt in the entry, after the flow returns and L1 is dropped. It is equivalent today, but step 4's flows would have to take it back in.
- A `bool` parameter in place of `Eoi`. `handle_irq(true)` says nothing at the call site.
- The flow answering whether it sent the end of interrupt, so that arch also covers a chip with none. It puts a check on every interrupt for a case `init_lines()` can refuse once, and it would hide a chip that is wrong, not catch it.
- Binding the directed EOI or `XAPIC_CHIP`: x86-apic items 2 and 3 stand, as the DESIGN's Staging says.
- Ending an edge interrupt before its actions: rejected in the DESIGN's Alternatives.

**Reference**
- Linux `handle_fasteoi_irq()` with `cond_unmask_eoi_irq()` and `cond_eoi_irq()` (`kernel/irq/chip.c:704-777`): the end of interrupt, `chip->irq_eoi()`, is the flow's last step, under `desc->lock`, including the path for a line with no action. `Desc::handle_irq()` follows it.
- Linux `ioapic_chip` (`arch/x86/kernel/apic/io_apic.c:1850-1858`): `.irq_eoi = ioapic_ack_level`, which ends the interrupt with `apic_eoi()`, the local APIC's EOI (`:1704`). It writes the IO-APIC's own EOI register, `eoi_ioapic_pin()`, only when a level interrupt arrived as an edge one. `ioapic_eoi()` is the first half of it.
- Linux `common_interrupt()` and `call_irq_handler()` (`arch/x86/kernel/irq.c:228-282`): `apic_eoi()` only when no descriptor takes the vector. `handle_irq_generic()` answering `bool` follows it.
- Zircon `platform_irq()` (`zircon/kernel/platform/pc/interrupts.rs:215-231`): `invoke_x86_vector()`, then `apic_issue_eoi()` for every platform vector, whether or not a handler was present. This is the arrangement ours had in `faults.cpp`, and the one this step leaves under the genirq exception (`references/task.md`, "Interrupt lines: kernel/irq follows genirq").
- The test entry has no counterpart in either. Linux tests its interrupt layer on a simulated domain instead, `irq_sim` (`kernel/irq/irq_sim.c:117-142`), whose chip has no end of interrupt and whose interrupts run `handle_simple_irq()`, a flow that ends nothing. `COUNTING_CHIP` plays the same part for `irq.test`'s own lines. The real lines keep their chip, and the tests skip the end of interrupt rather than swapping the chip.

**Verification**
- Formatting: `scripts/format_cpp.py` on every changed C++ file; it reformatted only lines this step changed.
- clang-tidy, with `-p out/bringup.x86`, on the eight touched sources (`desc.cpp`, `desc_table.cpp`, `mod.cpp`, `io_apic.cpp`, `faults.cpp`, `pc/interrupt.cpp`, `irq_test.cpp`, `interrupt_line_test.cpp`; the headers through them). No diagnostic falls on a changed line. In the touched files only three remain, all old `readability-identifier-naming` hits on untouched globals (`IOAPICS`, `IOAPIC_ISA_OVERRIDES`, `CURRENT_FRAME`).
- The build is `-Werror`, and it built clean.
- `boot_check.py`, one run: `dbg PASS` (34 passed, 2 skipped, 0 failed), `rel PASS` (30 passed, 10 skipped, 6/6 required), with `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A` in `rel.log`. The keyboard's line is edge-triggered: each key is an interrupt on GSI 1, vector 33, ended by the line's flow through `ioapic_eoi()`. A missing end of interrupt would have held back every key after the first.
- `kshell_run.py --out out/bringup.x86 irq.test interruptline.test interrupt.test`, one run: `irq.test: ok -- 39 checks` (33 before, plus the six above), `interruptline.test: ok -- 41 checks`, `interrupt.test: ok -- 32 checks`. `interruptline.test` raises its line by IPI on the line's vector again and again, through the interrupt entry and the line's flow. Each raise is only delivered once the one before has been ended, so the flow's end of interrupt is exercised there for real.
- Load: one boot_check run (two boots) and one kshell run; no repeated-boot stress.
- Not verified: that the local APIC's EOI clears a level pin's Remote IRR on this setup. No level line has fired yet (the DESIGN's Open questions).
- In review, against the DESIGN's Invariant and Lock order:
  - The lock order holds on every path. The end of interrupt is the only new step under L1, and `ioapic_eoi()` takes no lock: it writes this CPU's own local APIC. So dispatch stays L1 -> L2 -> {LI, L3}, with nothing taken under the end of interrupt. A later chip whose end of interrupt takes LI (the directed EOI) would add only L1 -> LI, which attach and configure already take.
  - The detach barrier is unchanged. The end of interrupt reads only the line's chip, never an action, and goes out inside the same hold of L1 as the actions, so nothing reaches an action after detach() returns.
  - The Permanent line (the tick) stays lockless and correct: its flow reads `m_data.chip`, fixed before the line is published, and the acquire in `is_permanent()` already orders the action list it walks.
  - Every caller of the old entries is updated: `git grep` finds no `notify()` on a line and no other caller of `handle_irq_generic()` or `platform_handle_irq()`.
  - One branch had no check. The false answer of `handle_irq_generic()`, arch ending a vector no line has, is reached by nothing at boot: with the test inverted, or the arch EOI dropped, both images would still boot and every test would pass. `interruptline.test` now raises by IPI vector `PlatformIrqMin + 200` (no line has number 200, and `irq::spurious_count()` answers none for it), then the free line's vector 35, both from one CPU with preemption off. Left in service, vector 232 (priority class 14) would hold back class 2 for good, and the free line's packet would never come. With the arch EOI removed for one run, the check reported `FAIL raised before the line's own, it is ended by the interrupt entry` and the CPU stayed wedged, as predicted; restored, it passes.
  - Review runs, after the fixups: build ok; `dbg PASS` (34 passed, 2 skipped, 0 failed) and `rel PASS` (30 passed, 10 skipped, 6/6 required), with `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A` in `rel.log`; `irq.test: ok -- 39 checks`, `interruptline.test: ok -- 43 checks` (41 plus the two above), `interrupt.test: ok -- 32 checks`; clang-tidy finds nothing in `interrupt_line_test.cpp`.
  - Review load: two boot_check runs (four boots) and four kshell runs, one of them on the mutant and one wasted on a build that had not run; no repeated-boot stress.

**Deliberately not done**
- D9 (c): `handle_irq_for_test()` turning preemption off, and `handle_irq_generic()` asserting interrupt context through `CURRENT_FRAME`. The DESIGN makes it a task of its own. Until then preemption stays the caller's, as the entry's comment says, and `interruptline.test`'s `fire()` keeps its own `AutoPreemptDisabler`.
- Flow selection, and masking a line with no action: step 4.
- `Chip::issue_eoi` keeps its name. The DESIGN's tables call the operation eoi, but renaming a member every chip initialises is churn beside this step's mechanism.
- `ioapic_issue_eoi()` stays as it is and stays bound to `issue_ack`, for x86-apic item 2; `XAPIC_CHIP` stays unbound while x86-apic item 3 stands.
- No log for a vector with no line. Linux logs one, rate-limited; here only a test's IPI could raise such a vector today.

**How it was found**
None.

**The user's prediction**
None made. The user took every recommendation and skipped the predictions ("直接做", 2026-10-05).

**Three questions you should be able to answer**
1. A platform vector is delivered and nobody ends it. Which interrupts does that CPU stop taking, and why is "never" the right word rather than "late"?
2. Why can arch not be the one to end the interrupt once step 4's level flow exists, and what does `handle_irq_generic()` answering false tell arch?
3. What could go wrong if a test played a level line's interrupt from a thread through the full flow, end of interrupt included, on a chip whose end of interrupt reaches the IO-APIC itself?

## Title: each line runs a flow chosen from how its pin signals, handle_fasteoi or handle_edge, and an interrupt on a line nobody is on masks the line (2026-10-05)

**Files**
- `kernel/include/ours/irq/types.hpp` — new `irq::Flow { Edge, FastEoi }`: which flow a line runs
- `kernel/include/ours/irq/desc.hpp`, `kernel/kernel/irq/desc.cpp` — `Desc` keeps `m_flow` under L1; `init()` chooses it from the chip's `trigger_mode`, `configure()` and `attach()` with a mode choose it again under L1 (`select_flow_locked()`); `handle_irq()` reads it under the same hold as the actions (without L1 on a Permanent line, through `permanent_flow()`) and runs it through `run_flow()`; new `handle_edge()` and `handle_fasteoi()`, which mask a line nobody is on (`mask_unclaimed()`) before ending its interrupt; new `flow()` for tests
- `kernel/kernel/irq/desc_table.cpp` — `init()` refuses a chip with no `mask` as well as one with no `issue_eoi`
- `kernel/include/ours/irq/chip.hpp` — `mask` and `trigger_mode` say that the flow uses them
- `kernel/include/ours/irq/mod.hpp` — `init_lines()`, `attach()` with a mode, `configure()`, `handle_irq_generic()` and `handle_irq_for_test()` say what happens to the flow and to a line nobody is on
- `kernel/commands/irq_test.cpp` — the counting chip also masks; a second chip over a pin kept in memory, with a mode and a mask bit; eleven new checks, on flow selection and on what each flow does to the pin
- `kernel/commands/irq_test.cpp` — the pin also counts the ends of interrupt sent while it was unmasked, so the masking check sees that a line nobody is on is masked before its interrupt is ended (added in review)
- `kernel/kernel/irq/desc.cpp` — `handle_irq()`'s comment counts the line's flow among what a Permanent line reads without L1 (added in review)
- `.agents/tasks/irq-genirq/PLAN.md` — step 4

**Symptom**
Latent, cannot be triggered today. Lines with an action behave as they did, and no level line fires on QEMU's q35. Before this step every line ran one sequence, the actions and then the end of interrupt, so an interrupt on a line nobody was on was counted as spurious, ended, and left unmasked. A level pin whose device holds it asserted, unmasked with nobody on the line, would then interrupt the CPU it targets again the moment each interrupt was ended, for good: a storm counted one spurious interrupt at a time, which keeps that CPU in interrupt context. Nothing unmasks such a line today. The platform programs every pin masked, an owner unmasks only after it attaches, and an interrupt object masks its line itself before it detaches (`interrupt_dispatcher.cpp:290`). But nothing in kernel/irq made that so: an `irq::unmask()` of a line nobody is on, such as `irq.test`'s mask, unmask and mask again on the idle line, opened the window.

**Cause**
The objects:
- A level pin: asserted by its device until the device is serviced. The IO-APIC delivers it once and sets the entry's Remote IRR, and delivers nothing more from the pin until an EOI for its vector clears Remote IRR. If the pin is still asserted and unmasked then, it delivers again at once.
- An edge pin: one event per signal. While the vector is in service on the local APIC, one more edge waits in the local APIC's IRR and comes in once the first is ended.
- The line's flow (`Desc::handle_irq()` until this step, now `handle_fasteoi()` or `handle_edge()`): the order in which an interrupt on the line masks the line, runs its actions and ends the interrupt. It runs under L1, the line's lock, or without L1 on a Permanent line.
- The chip's `trigger_mode`: reads how the pin signals from its IO-APIC redirection entry, the one record of the pin's mode.
- `configure()` and `attach()` with a mode: the only two ways the mode of a line in the table changes once the table is built, both under L1, both refused on a line someone is on.

Why a line needs a flow of its own, and why the flow depends on the trigger mode:
- What a line must do around its actions depends on how its pin signals. A level pin is re-raised by the controller as soon as its interrupt is ended, for as long as its device is not serviced. An edge pin is not. So the steps a line takes before and after its actions differ by mode, and kernel/irq can only take them if it knows each line's mode at the moment of the interrupt. Today the one place they differ is a line nobody is on; from step 6 the Oneshot mask held across a level line's handling is a second.
- The mode could be asked of the chip on every interrupt (D8 (b)). On the IO-APIC that is an IOREGSEL and IOWIN pair under LI in every dispatch, to learn something that changes only when an owner sets it. D8 (a) chooses the flow where the mode is learnt or set instead: when `init_lines()` binds the line to its chip, which the platform does after it has programmed every pin; and in `configure()` and `attach()` with a mode, the only later writers. So the flow stays in step with the pin without asking it. `attach()` with no mode reads the mode anyway, to answer it, and chooses the flow from what it read, so a pin programmed behind the line's back is caught at the latest when an owner takes it.
- Why the flow is chosen under L1. `handle_irq()` reads it under the same hold as the action list, so a `configure()` or `attach()` that changes it lands wholly before an interrupt or wholly after, and an action's first interrupt runs the flow of the mode it took the line in. Without the lock the read would race `configure()`'s write, which is a data race and so undefined behaviour, whatever the size of the field.
- Why a Permanent line may read it without L1. `configure()` and `attach()` with a mode both refuse a Permanent line, so its flow, like its list, never changes again once Permanent is set. The release store that sets Permanent comes after the last write of the flow, and the dispatch's acquire load that sees Permanent therefore sees that write. This is the argument the list already rested on.

Why an interrupt on a line nobody is on masks the line, and why before it is ended:
- For a level pin: the end of interrupt clears Remote IRR, and if the pin is still asserted and unmasked it is delivered again at once. Nobody is on the line to service the device, so nothing will ever deassert it. Masking first and ending second leaves the pin masked with Remote IRR clear: nothing more is delivered, and when an owner attaches and unmasks, the still-asserted pin delivers its interrupt to the owner, so nothing is lost. Ending first and masking second leaves a window in which the pin delivers again, to another CPU or to this one once its interrupts are back on.
- For an edge pin: the device that sent the edge may keep sending, each edge an interrupt that does nothing but count. Masking stops that until an owner unmasks. An edge that arrives while the pin is masked is not replayed when it is unmasked; resend is D5's, and deferred. Before this step such an edge was ended and lost all the same.
- The interrupt is still counted as spurious, so `irq.test`'s and `interruptline.test`'s spurious checks keep their meaning. A line whose actions all answer None is only counted, as before: stuck-line disabling is deferred.
- `init_lines()` now refuses a chip that cannot mask. The masking is the guarantee that a line nobody is on stays quiet, and the table checks it once, as it checks the eoi since step 3, instead of the flow checking on every interrupt. The flow keeps a `DEBUG_ASSERT` for a line of a test's own, outside the table.
- A played dispatch (`handle_irq_for_test()`) masks a line nobody is on too, at the real controller. It is thread context, and LI is taken with IrqSave, so this is safe. It leaves the line as a real interrupt would: on the idle line the tests use, which is masked already, it changes nothing.

Why the two flows are the same today for a line with an action:
- "Lines with an action behave as today" is this step's boundary. An interrupt object on a level line still masks the line itself from its handler and unmasks it at the ack or the wait that serves as one (`interrupt_dispatcher.cpp:238-240`, `:263-265`, `:410`, `:119`), so the flow must not touch the mask of a line with an action yet. Two writers of one mask bit is what step 7 removes, and step 6 is where `handle_fasteoi()` gains the Oneshot mask. The two functions are kept apart now so that step 6 is a change to one of them rather than a split.

Why an enum and a switch, not a pointer to the flow as genirq keeps one:
- genirq stores a function pointer in the descriptor and calls through it, because chips, platforms and drivers install flows of their own: chained, per-CPU, nested, simple. ours has two flows, both kernel/irq's. An enum of two values is all the line needs to record, the switch is a direct call rather than an indirect one, and a test can read the line's flow back and compare it, which it cannot do with a pointer to a private member function.
- The cost is that a third flow is a change to kernel/irq. That is where a flow belongs while the chips are kernel/irq's own, and the enum can become a pointer when a chip outside it needs a flow of its own, for example an MSI chip with D4's domain.

**Change**
The dispatch, before and after:

```cpp
// before
auto Desc::handle_irq(Eoi const eoi) -> IrqReturn {
    auto result = IrqReturn::None;
    if (is_permanent()) {
        result = notify_all(permanent_actions());
        end_irq(eoi);
    } else {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        result = notify_all(m_actions);
        end_irq(eoi);
    }
    // spurious count ...
}
```

```cpp
// after
auto Desc::handle_irq(Eoi const eoi) -> IrqReturn {
    auto result = IrqReturn::None;
    if (is_permanent()) {
        result = run_flow(permanent_flow(), permanent_actions(), eoi);
    } else {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        result = run_flow(m_flow, m_actions, eoi);
    }
    // spurious count ...
}

auto Desc::handle_fasteoi(Action::List &actions, Eoi const eoi) -> IrqReturn {
    if (actions.empty()) {
        mask_unclaimed(); // before the end of interrupt, so the pin cannot deliver again
        end_irq(eoi);
        return IrqReturn::None;
    }
    auto const result = notify_all(actions);
    end_irq(eoi);
    return result;
}
// handle_edge() has the same steps today; run_flow() switches on the Flow.
```

The flow is chosen in three places, all under L1:

```cpp
auto Desc::select_flow_locked(TriggerMode const trigger) -> void; // Level: FastEoi, else Edge

// Desc::init(chip): from chip.trigger_mode(), Edge for a chip that cannot say
// Desc::configure(): after chip->configure() answers Ok; a refusal leaves the flow
// Desc::attach(action, mode): from the mode set, or read, before the action is linked
```

`irq.test` gains a chip over a pin of the test's own kept in memory (`PIN_CHIP`: its mode, its mask bit, and counts of masks and ends of interrupt). The counting chip also masks, onto the same pin. Eleven new checks, on lines of the test's own:
- binding a line gives it the flow of how its pin signals, FastEoi for a level pin and Edge for an edge pin;
- a chip that cannot say how its pin signals gives the edge flow;
- `configure()` moves the flow with the mode, there and back;
- a `configure()` refused on an occupied line leaves the flow;
- `attach()` with a mode gives the line that mode's flow;
- `attach()` without one reads the mode and gives its flow;
- the two lines of the masking checks run the level and the edge flow;
- an interrupt on a line nobody is on masks it and ends it, on either flow;
- a played one masks it too and ends nothing;
- a line with an action is neither masked nor unmasked by its flow, on either flow, with its pin unmasked and with it masked;
- the line table refuses a chip that cannot mask.

Rejected:
- Asking the chip for the mode on every interrupt (D8 (b)): see Cause.
- Not masking the edge line nobody is on, since an edge does not storm by itself: a device that keeps sending makes it do the same work, and genirq masks on both flows.
- A chip without a mask leaving a line nobody is on unmasked, silently: the guarantee would then hold only for some chips. The table refuses such a chip instead.
- A table-level `irq::flow(VIrqNum)` for the tests: it would add to `mod.hpp` for tests alone. The selection is `Desc::init()`'s, which `DescTable::init()` calls for every line, so lines of the test's own reach it. The real lines' masks are read back at step 5 (D10).

**Reference**
- Linux `handle_fasteoi_irq()` (`kernel/irq/chip.c:740-777`): when `irq_can_handle_actions()` (`:548-557`) finds no action, `mask_irq(desc)` and then `cond_eoi_irq()`, under `desc->lock`. `Desc::handle_fasteoi()` follows it. Linux also sets `IRQS_PENDING` there, to replay the interrupt later; ours does not (D5).
- Linux `handle_edge_irq()` (`kernel/irq/chip.c:827-862`): with no action, `mask_ack_irq()` and return. `Desc::handle_edge()` masks and then ends the interrupt after, as it ends every interrupt after the actions (the DESIGN's Alternatives).
- Linux `mp_register_handler()` (`arch/x86/kernel/apic/io_apic.c:845-859`): `handle_fasteoi_irq` for a level IO-APIC pin, `handle_edge_irq` otherwise, named "fasteoi" and "edge". It runs when the pin's interrupt is allocated, `mp_irqdomain_alloc()` (`:2898`), and again from `mp_check_pin_attr()` (`:862-880`) when the first user of a legacy pin sets a different trigger. `Desc::init()` and `attach()` with a mode follow these two.
- Linux `irq_set_handler_locked()` (`include/linux/irqdesc.h:214-229`): sets `desc->handle_irq` with `desc->lock` held, "typical call site is the irq_set_type() callback", which `__irq_set_trigger()` (`kernel/irq/manage.c:925`) calls, from `__setup_irq()` (`:1718`) among others. `select_flow_locked()` under L1, from `configure()` and `attach()` with a mode, follows it.
- Linux `generic_handle_irq_desc()` (`include/linux/irqdesc.h:186-189`): `desc->handle_irq(desc)`, a call through the pointer. `Desc::run_flow()` switches on an enum instead (see Cause).
- Zircon has no flows. `InterruptTableEntry::invoke_if_present()` (`zircon/kernel/platform/pc/interrupt_manager.rs:90-114`) answers false for a vector with no handler, and `platform_irq()` (`zircon/kernel/platform/pc/interrupts.rs:215-231`) ends it all the same and masks nothing. ours followed that until this step, under the genirq exception recorded in step 1 (`references/task.md`, "Interrupt lines: kernel/irq follows genirq").

**Verification**
- Formatting: `scripts/format_cpp.py` on every changed C++ file.
- clang-tidy, with `-p out/bringup.x86`, on `desc.cpp`, `desc_table.cpp`, `mod.cpp` and `irq_test.cpp`, the headers through them: no diagnostic in an irq header or in `irq_test.cpp`; the one left in the touched files is the old `CURRENT_FRAME` naming hit in `mod.cpp`.
- The build is `-Werror` and the thread-safety analysis is on: `m_flow` is `TA_GUARDED(m_lock)` and every write is under it, and the lockless read on a Permanent line is the one marked `TA_NO_THREAD_SAFETY_ANALYSIS`, beside `permanent_actions()`.
- `boot_check.py`: `dbg PASS` (34 passed, 2 skipped, 0 failed) and `rel PASS` (30 passed, 10 skipped, 6/6 required), with `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A` in `rel.log`. The keyboard's line is edge-triggered and has an action, so it runs `handle_edge()`'s claimed path on every key; the tick's Permanent line runs it without L1.
- `kshell_run.py --out out/bringup.x86 irq.test interruptline.test interrupt.test`: `irq.test: ok -- 50 checks` (39 before, plus eleven), `interruptline.test: ok -- 43 checks`, `interrupt.test: ok -- 32 checks`. `interruptline.test`'s destroy check plays an interrupt on the free line after its object has gone, which now masks the real pin, and the next create still takes the line and unmasks it.
- The barrier check's dispatch loop now masks the idle line's real pin on every dispatch that finds the line empty: three runs counted 766, 813 and 958 handled and 29725, 30381 and 31551 empty dispatches over their 300 rounds, where step 3's record counts more than 166000 played dispatches over a whole `irq.test` run, almost all of them this loop's. Every round still has a handler to race (`a dispatch on another CPU reaches the action`), so the check still tests the barrier. A real interrupt pays the mask only on a line nobody is on.
- The dbg image's shell finished at 5.0 s, 38.0 s, 32.8 s and 35.4 s in the four dbg boots of this step, and at 4.4 s, 56.3 s and 57.1 s in three dbg boots of the step-3 commit built into `out/base` for comparison. The slow boots spend the time in user space, preempted by the local APIC timer while the dbg image's debug-level threads print, on the step-3 commit as well, so the variation is not this step's. Not investigated further.
- Load: three boot_check runs (six boots), one more dbg boot of this step, three dbg boots of the step-3 commit and three kshell runs; no repeated-boot stress.
- Not verified: that a masked IO-APIC pin keeps a still-asserted level interrupt and delivers it on unmask, and that the local APIC's EOI clears Remote IRR, since no level line fires on q35 (the DESIGN's Open questions). Step 5 reads the real pin's mask bit back.
- In review, against the DESIGN's Invariant and Lock order:
  - The lock order holds on every path. The one new acquisition is LI under L1: the flow's mask of a line nobody is on, and `Desc::init()` reading the mode. attach and configure already take L1 -> LI. LI is taken with IrqSave everywhere, and the objects' own mask already took it in interrupt context, so taking it from the flow adds no edge and no new context. Nothing is taken under the end of interrupt.
  - The detach barrier is unchanged. The flow is read and run inside the one hold of L1 that the actions run under. `mask_unclaimed()` reads only the chip, never an action.
  - The Permanent line (the tick) stays lockless and correct. `m_flow` is written by `init()`, by `configure()` and by `attach()` with a mode. The last two refuse a Permanent line, and when an attach with a mode links a Permanent action, the release store in `link_locked()` comes after its write of the flow. So the acquire in `is_permanent()` orders every write that `permanent_flow()` can see. The tick's line, GSI 2, is edge, and every boot dispatches it through `permanent_flow()` and `handle_edge()` without L1.
  - The flow's mask cannot undo an owner's unmask. A dispatch that finds the line empty holds L1 from the check through the mask, and attach takes L1, so that mask lands before any attach, and an owner unmasks only after its attach returns. An interrupt object unmasks at its ack under L2, and only while it is not Destroyed; destroy sets Destroyed under L2, then masks, then detaches.
  - Every caller is updated. The IO-APIC's chip is the only chip a table is built with, and it has a mask. Every test chip that a table or a dispatch with nobody on the line reaches has one too. `XAPIC_CHIP` stays unbound.
  - The comment rule holds: `grep -i` finds no Linux, Zircon, Fuchsia or genirq in the irq headers, the irq sources or the two tests.
  - One claim had no check: the order. The Cause rests on "mask, then end" for a level pin, but the masking check compared only counts (one mask, one end, masked at the end). So a `handle_fasteoi()` that ended the interrupt first would have passed all 50 checks. The pin chip now also counts the ends of interrupt sent while the pin was unmasked. The check starts the pin unmasked and requires no such end for a line nobody is on. Run once against that mutant, `irq.test` reported `FAIL an interrupt on a line nobody is on masks it, then ends it, on either flow`; restored, it passes.
  - The dbg shell's slow and varying finish time is `docs/repairs/261005/kernel-top.md` item 1, already recorded. The review's dbg boot finished at 52.2 s.
  - Review runs, after the fixup: build ok; `dbg PASS` (34 passed, 2 skipped, 0 failed) and `rel PASS` (30 passed, 10 skipped, 6/6 required), with `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A` in `rel.log`; `irq.test: ok -- 50 checks` (the barrier: 809 handled, 29717 empty), `interruptline.test: ok -- 43 checks`, `interrupt.test: ok -- 32 checks`. `format_cpp.py --check` is clean, and clang-tidy finds nothing in `desc.cpp`, `irq_test.cpp` or an irq header.
  - Review load: one boot_check run (two boots) and two kshell runs, one of them on the mutant; no repeated-boot stress.

**Deliberately not done**
- The Oneshot mask on a level line with an action, `WakeThread` and held: step 6. The objects keep masking their own level line until step 7.
- `IRQS_PENDING` and resend for an interrupt that found nobody on the line: D5 (c).
- A Masked bit in the descriptor: the DESIGN's Non-goals keep the IO-APIC entry as the one record of the mask.
- A test of the real table's flows and masks: the mask read-back is step 5 (D10 (b)), and the table's selection is `Desc::init()`'s, tested on lines of the test's own.
- The ack before the actions on an edge line, as genirq's `handle_edge_irq()` does: rejected in the DESIGN's Alternatives.
- A name for each flow in the descriptor, as genirq keeps one for its interrupt listing: no listing exists; `irq.test` names them for its log.

**How it was found**
None.

**The user's prediction**
None made. The user took every recommendation and skipped the predictions ("直接做", 2026-10-05).

**Three questions you should be able to answer**
1. A level pin's device holds it asserted and nobody is on the line. Walk through what the IO-APIC and the local APIC do if the flow ends the interrupt and then masks, and if it masks and then ends. Which order loses nothing for the next owner, and why?
2. Why can the flow of a line in the table be chosen at three moments rather than read from the chip on every interrupt, and what would have to change for that to stop being true?
3. Why may the tick's Permanent line read its flow without L1, while every other line must read it under L1? Which release and which acquire make it safe?

## Title: a line's mask is read back from its controller for the kernel tests, and interruptline.test checks that a level object's line is masked from delivery to its ack and an edge object's never is (2026-10-05)

**Files**
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/ioapic.hpp` — `IoApicRouteEntry::masked()` and `IoApic::is_masked()`: the mask bit, bit 16, of a pin's redirection entry, read like `trigger_mode()` reads bit 15
- `kernel/arch/x86/apic/io_apic.cpp` — new `ioapic_is_masked()`, under LI, bound as the IO-APIC chip's `is_masked`
- `kernel/include/ours/irq/chip.hpp` — new optional `Chip::is_masked`, for kernel tests only
- `kernel/include/ours/irq/desc.hpp`, `kernel/kernel/irq/desc.cpp` — new `Desc::is_masked()`, through the chip, without L1; NotSupported for a chip that cannot say
- `kernel/include/ours/irq/desc_table.hpp`, `kernel/kernel/irq/desc_table.cpp` — new `DescTable::is_masked()`, with `find_line()`'s answers
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp` — new `irq::is_masked_for_test()`, the one public entry
- `kernel/commands/interrupt_line_test.cpp` — `line_masked()` and `line_unmasked()`; twelve new checks in `check_level_mask_unbound()`, `check_level_mask_bound()` and `check_edge_mask()`, run after `check_modes()`; the header says what is read back instead of what cannot be seen
- `kernel/commands/irq_test.cpp` — `check_line_control()` reads the idle line's pin back after its mask, unmask and mask, and checks the read's answers for no line and for a line with no chip: two new checks, one widened
- `docs/repairs/261005/object.md` — item 1, no test sees a level line masked until its ack, removed with its option (b); items 2 to 5 are 1 to 4 now
- `.agents/tasks/irq-genirq/PLAN.md` — step 5

**Symptom**
Latent, cannot be triggered today: nothing was wrong with the objects, but no test could have told if it were. `interruptline.test` raises its interrupts by IPI on the line's vector or plays them from a thread, and both go around the IO-APIC. Nothing read a redirection entry's mask bit back. So any of these would have passed every check:
- a level object that never masked its line;
- one that masked it and never unmasked it, so its second interrupt never came;
- an edge object that masked its line until the ack, losing every edge sent meanwhile.

That gap was `docs/repairs/261005/object.md` item 1. It matters now because steps 6 and 7 move a level line's mask out of the object and into the line's flow, and the promise of D1 (b) is that the user sees no difference. Without a read of the pin, the move could not be checked against that promise.

**Cause**
The objects:
- The pin's redirection entry in the IO-APIC. Bit 16 is the mask: set, the pin raises nothing. It is the one record of whether the pin is masked, by the DESIGN's choice: kernel/irq keeps no software Masked bit (Non-goals), and `mask()` and `unmask()` write the entry and nothing else.
- `fire()`: a dispatch of the line from a thread, through `irq::handle_irq_for_test()`. No interrupt is raised at all.
- `raise()`: an IPI on the line's vector to this CPU. It is delivered by the local APIC, so the IO-APIC entry, and its mask, are never consulted.
- A level object's mask protocol today (`interrupt_dispatcher.cpp`): the handler masks the line when it delivers to a waiter (MaskPostwait), and `send_packet_locked()` masks it when a packet goes out to a port. The line is unmasked at the ack (UnmaskPrewait): `ack()` when bound, or the next wait when unbound, which serves as the ack.

Why the existing checks could not see the mask:
- What a test observes of an object is its wakeups and its packets, and both come from the handler whether the pin is masked or not. The tests' interrupts never pass through the pin, so masking it changes nothing they deliver.
- The mask is a promise about interrupts from the device, which the tests do not have. QEMU's q35 has no free level line that a device raises before PCI.
- So the effect of the protocol is visible only in the register itself, and only a read of the register can check it.

Why read the controller, rather than keep a bit in kernel/irq for the test to read:
- A software bit would be a second record of the mask, and its writers would hold no common lock (the DESIGN's Alternatives). Until step 7 the object writes the mask under its own L2: inside the dispatch's L1 from its handler, but without L1 at its ack. The flow writes it under L1 alone, for a line nobody is on, and the HPET's and the tests' `irq::mask()` under neither.
- A test reading that bit would check the bookkeeping, not the pin. A mask that never reached the controller, for example because a chip's mask wrote the wrong entry, would still pass.
- The read goes through the chip because the chip is the code that knows where the bit lives. `trigger_mode()` already reads the same entry the same way, under LI.

Why this option and not a real level device (D10):
- Option (a) was the ACPI power-management timer's SCI on GSI 9. It needs the FADT's PM1 event block decoded, which the kernel does not do yet, and whether QEMU raises the SCI for the timer's overflow event is unchecked.
- Option (b), this change, checks the real register on the free line, in ModeLevelHigh, with nothing else to build. It also checks the register both before step 7 and after it. Today the object writes the mask; after step 7 the flow writes it. The same twelve checks must pass on both sides of that switch, which makes them the regression test for steps 6 and 7.
- The cost is an interface with no caller but the tests. It is named `is_masked_for_test()`, as `handle_irq_for_test()` is, so that a caller on the interrupt path stands out. The chip's operation is optional: a chip that cannot read its mask leaves it null, and the read answers NotSupported.

Why each check reads where it does:
- A read answers for the moment it is taken. On the free line nobody else masks or unmasks: the object holds the line exclusively and no device is behind it, so the test thread alone drives the pin, and each read follows a call that has returned.
- A `fire()` masks before it returns, and so does an `ack()`. The unbound "ack" is the waiter's next wait, which unmasks before it parks, so the test reads only once `park_waiter()` has seen the thread parked.
- For `raise()` the test reads after the packet arrives. Today `send_packet_locked()` queues the packet and then masks, both under L2, in the handler on this CPU. The test thread is on this CPU and the IPI is taken at once, so the handler has finished before the thread polls. The read would race the mask only if the thread moved to another CPU between the send and the IPI's delivery. After step 7 even that window is gone, since the flow masks the line before the object's handler queues anything.

What the edge check stands for:
- An edge is one event. Nothing re-raises it, so there is nothing to hold off while a driver services the device.
- A masked edge pin drops the edges that arrive meanwhile; whether the IO-APIC keeps one is the DESIGN's open question, and nothing replays it (D5).
- So an edge object that masked its line until the ack would lose interrupts. "Never masked" is the edge half of the protocol, and it has to survive step 7 too, where `handle_edge()` ignores Oneshot.

**Change**
The chip gains one optional operation, and kernel/irq passes it through to one public entry:

```cpp
// kernel/include/ours/irq/chip.hpp, after trigger_mode
auto (*is_masked)(Data const &) -> bool = nullptr; // for kernel tests only

// kernel/include/ours/irq/mod.hpp
[[nodiscard]]
auto is_masked_for_test(VIrqNum virqnum) -> ktl::Result<bool>;
// NoResource for no line, InvArgs for INVALID_V_IRQ_NUM,
// NotSupported if the chip cannot say: the answers of trigger_mode()

// kernel/kernel/irq/desc.cpp: no L1, as trigger_mode()
auto Desc::is_masked() const -> ktl::Result<bool> {
    auto const *const chip = m_data.chip;
    if (!chip || !chip->is_masked) {
        return ustl::err(Status::NotSupported);
    }
    return ustl::ok(chip->is_masked(m_data));
}
```

The IO-APIC chip reads the entry under LI, as `ioapic_trigger_mode()` does:

```cpp
// kernel/arch/x86/apic/io_apic.cpp
auto ioapic_is_masked(irq::Data const &data) -> bool {
    auto &ioapic = owner_of(data.hirqnum);
    Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
    return ioapic.is_masked(data.hirqnum); // read_redirection(irq).masked(): bit 16
}
```

`interruptline.test` gains twelve checks on the free line, read through `irq::is_masked_for_test()` after each step:
- Level, unbound (`check_level_mask_unbound()`, five checks): the line is unmasked once created; `fire()` masks it; it stays masked once a wait has taken the interrupt; the next wait, with nothing pending, is the ack, and unmasks it before parking; a `fire()` with that wait parked masks it again and wakes the wait.
- Level, bound to a port (`check_level_mask_bound()`, six checks): `fire()` masks the line as its packet goes out; a second `fire()` before the ack, then an `ack()` that sends it, leaves the line masked; the next `ack()`, with nothing to send, answers Ok and unmasks it; a `raise()` through the interrupt entry masks it as a dispatch does, and its ack unmasks it.
- Edge (`check_edge_mask()`, one check): the line reads unmasked after the create, an interrupt, the wait that takes it, the wait that acks it and takes the next, a bound interrupt's packet, an ack that sends the interrupt since, the ack with nothing to send, a raised interrupt and its ack. Each read that fails is logged by name. Created in ModeEdgeHigh, it also leaves the line edge, as `irq.test` expects to find it.

`irq.test`'s `check_line_control()` reads the idle line's pin after its mask, unmask and mask: masked, unmasked, masked. It also checks that the read answers NoResource for no line, and NotSupported for a line with no chip. Its comment no longer says that only the trigger mode can be read back.

Rejected:
- A software Masked bit in `Desc`: see Cause. The DESIGN defers it with lazy disable.
- `irq::is_masked()` without the suffix: it would invite a caller on the interrupt path to ask the controller for something the flow is meant to know, at an MMIO read under LI each time.
- Reading the mask in `irq.test` alone, on the idle line nobody is on: that checks the chip, not the objects, whose protocol is what steps 6 and 7 change.
- A destroy check, that a destroyed object leaves its line masked: outside this step's checks. It is true today, through the object's own mask in `destroy()`, and from step 7 it falls to detach (the DESIGN's Events, 8), which is the step to add it in.

**Reference**
- Linux `irq_get_irqchip_state()` (`kernel/irq/manage.c:2730`) with `IRQCHIP_STATE_MASKED` (`include/linux/interrupt.h:502`). It reads a line's state from the controller through the chip's optional `irq_get_irqchip_state` operation (`__irq_get_irqchip_state()`, `:2695-2717`), and that is the shape `Chip::is_masked` follows: a chip operation that asks the hardware and that a chip may leave out. Linux calls it for KVM's forwarded interrupts, and for `__synchronize_hardirq()` (`:77`) with IRQCHIP_STATE_ACTIVE; ours calls it only from tests.
- Linux `ioapic_irq_get_chip_state()` (`arch/x86/kernel/apic/io_apic.c:1823-1850`) reads the pin's redirection entry under `ioapic_lock`, as `ioapic_is_masked()` reads it under LI. It answers only IRQCHIP_STATE_ACTIVE, from the Remote IRR bit, since Linux's own record of the mask is in software.
- Linux `IRQD_IRQ_MASKED` (`include/linux/irq.h:238`, read by `irqd_irq_masked()` at `:346`), set and cleared by `mask_irq()` and `unmask_irq()` (`kernel/irq/chip.c:432`, `:443`) and shown in debugfs (`kernel/irq/debugfs.c:105`). That is where a Linux test would look. ours keeps no such bit (the DESIGN's Non-goals), so its test reads the register.
- Zircon has no read of a line's mask: `InterruptManager::mask_interrupt()` and `unmask_interrupt()` (`zircon/kernel/platform/pc/interrupt_manager.rs:201-210`) only write the entry, and so do the object's `MaskInterrupt()` and `UnmaskInterrupt()` calls around delivery and the ack (`zircon/kernel/object/interrupt_dispatcher.cc:245`, `:394`, `:430`), which today's objects follow.

**Verification**
- Formatting: `scripts/format_cpp.py` on every changed C++ file, nothing to change.
- clang-tidy, with `-p out/bringup.x86`, on `desc.cpp`, `desc_table.cpp`, `mod.cpp`, `io_apic.cpp`, `irq_test.cpp` and `interrupt_line_test.cpp`, the headers through them: nothing on a changed line. The hits left in the touched files are the old naming ones (`IOAPICS`, `IOAPIC_ISA_OVERRIDES`, `CURRENT_FRAME`, the `IoApic` members' trailing underscores).
- `boot_check.py`: `dbg PASS` (34 passed, 2 skipped, 0 failed; the shell finished at 56.2 s, `docs/repairs/261005/kernel-top.md` item 1) and `rel PASS` (30 passed, 10 skipped, 6/6 required), with `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A` in `rel.log`.
- `kshell_run.py --out out/bringup.x86 irq.test interruptline.test interrupt.test`: `irq.test: ok -- 52 checks` (50 before, plus two), `interruptline.test: ok -- 55 checks` (43 before, plus twelve), `interrupt.test: ok -- 32 checks`.
- The checks bite: one run of `interruptline.test` against a mutant whose create gives the mask flags to edge objects instead of level ones (`!=` for `==` in `interrupt_event_dispatcher.cpp`) answered `FAILED -- 7 of 55 checks`. The six level checks that need the line masked failed, and so did the edge check, with six of its reads logged as not unmasked. The checks that need the line unmasked after an ack still passed there, since a line nobody masked reads unmasked anyway: they bite only on an object that masks and fails to unmask. Restored, the source matches what was booted.
- Load: one boot_check run (two boots) and three kshell runs. One of those runs booted the unmutated kernel by mistake, since the mutant build went to the `prebuilt/ninja` directory instead of the binary. No repeated-boot stress.
- Not verified: a real level interrupt held back by the mask, since none fires on q35; that is still the DESIGN's open question on Remote IRR.

**Deliberately not done**
- A destroy check: see Rejected; it belongs with step 7's detach.
- A read of Remote IRR, the IO-APIC's "level interrupt in service" bit, for the open question on whether the local APIC's EOI clears it: no level interrupt fires on q35, so it would always read clear.
- An `is_masked` for `XAPIC_CHIP` or for `irq.test`'s pin chip: no table binds the first, and the pin chip's mask is read directly from its in-memory pin.

**How it was found**
None: step 5 of the DESIGN's Staging, closing `docs/repairs/261005/object.md` item 1.

**The user's prediction**
None made. The user took every recommendation, D10 (b) included, and skipped the predictions ("直接做", 2026-10-05).

**Three questions you should be able to answer**
1. `interruptline.test` already showed a level object's interrupts arriving, folding and being acked. Why could none of those checks tell whether the object masked its pin, and what would a level device on a real machine have done if it did not?
2. Why does the read go to the IO-APIC entry instead of to a bit kernel/irq keeps, and which of today's paths would write such a bit without the lock the others hold?
3. After step 7 the flow masks a level line and the object only reports when its debt is cleared. Which of the twelve checks change the code path they exercise, and why must every one of them still pass?
