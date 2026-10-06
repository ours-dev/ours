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
