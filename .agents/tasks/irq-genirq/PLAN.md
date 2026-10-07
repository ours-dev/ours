# irq-genirq: the steps that rebuild kernel/irq around line descriptors, flows and ONESHOT

**Date**: 2026-10-05
**Status**: Accepted

## Request
On 2026-10-05 the user chose to rebuild kernel/irq in the style of Linux's genirq, and asked which of its responsibilities could move into the interrupt objects (DESIGN.md, Problem). Shown the design, they answered "直接做": every decision D1-D10 is taken as recommended. The prediction for D1 and the concurrency argument for D2 are skipped; the finalize exercise and interleaving B stay open in DESIGN.md's Open questions.

The scope ends at DESIGN.md's Goals: genirq's roles at "basically usable", one EOI per vector through the chip, ONESHOT owned by the line, and interrupt objects that no longer write the controller mask. Its Non-goals and its Deferred list are out of scope.

## Decisions
None. The plan follows DESIGN.md's decisions D1-D10, all taken as recommended on 2026-10-05.

## Steps
The steps keep DESIGN.md's Staging numbers. Each joins this list when it starts, as the Staging says, so a step not listed here has not started.

Every step:
- lands as one commit, made with `git commit -- <paths>`, with its entry in `docs/changes/<YYMMDD>/kernel-irq.md` in the same commit;
- builds both images and boots them under `python3 .agents/skills/ours-verify/scripts/boot_check.py`, and the rel image still logs `pc-ps2: key a` and `pc-ps2: key A`;
- then runs `python3 scripts/kshell_run.py --out out/bringup.x86 irq.test interruptline.test interrupt.test` on dbg, every test ok.

### 1. Record the exception
- What: `ours-kernel-design`'s `references/task.md` gains "Interrupt lines: kernel/irq follows genirq"; the skill's index points interrupt-line work at it; DESIGN.md is Accepted, with every Decided cell filled.
- Why now: every later step departs from Zircon, the skill's default spec. Without the rule beside that default, a reviewer of step 3 or step 7 would revert the change as drift.
- Verified: `python3 scripts/lint_docs.py --changed` on the documents. Docs only; in review both images were built and booted once (dbg and rel PASS, rel with both `pc-ps2: key` lines) and the three kernel tests passed, as the baseline for step 2.
- Module: `kernel-irq`.
- Commit: `docs(irq): accept the genirq design and record kernel/irq as a Linux exception` (933d0004), with the review's fixup.

### 2. Renames per D6
- What: kernel/irq's types take genirq's role names in the scaffold's short style, and their files the names of what they hold: `IrqObject` becomes `irq::Desc` (`desc.hpp`, `desc.cpp`); `IrqObserver` becomes `irq::Action`, filling the `action.hpp` placeholder; `IrqChip` and `IrqData` become `irq::Chip` and `irq::Data` (`chip.hpp`); `irq::IrqDispatcher` becomes `irq::DescTable` (`desc_table.hpp`, `desc_table.cpp`), as the DESIGN's Objects table names it. Variables, members and comments that said observer say action, the interrupt object's `m_observer` included (now `m_action`, the name step 7's finalize call uses). Every caller follows: the interrupt objects, the IO-APIC and local APIC chips, the PIT's line in `time.cpp`, `irq.test`, and the open repair items that name the old types. `domain.hpp` stays empty, as D4 keeps it. No behaviour change.
- Why now: every later step adds to these types (the flows to `Desc`, Oneshot to `Action`, the eoi and the mask read-back to `Chip`). Renaming first keeps each of those diffs about its mechanism, and the names match what the user reads in genirq from the first mechanism on.
- Verified: format_cpp and format_gn on every changed file; clang-tidy on every touched source shows nothing new on a changed line; the build and both boots; the three kernel tests with the same check counts as the baseline.
- Module: `kernel-irq`.
- Commit: `refactor(irq): name kernel/irq's types after their genirq roles` (e1ac6527), with the review's fixup.

### 3. EOI through the chip
- What: the IO-APIC chip's `issue_eoi` becomes `ioapic_eoi()`, the local APIC's EOI, and `irq::Desc::notify()` becomes `irq::Desc::handle_irq(Eoi)`, the line's one flow: the actions, then the end of interrupt through the chip, inside the same hold of L1 (without L1 on a Permanent line). `irq::handle_irq_generic()` answers whether a line took the interrupt, and arch sends the local APIC EOI itself only when none did. `irq::handle_irq_for_test()` runs the same flow with `Eoi::Skip`, and `irq.test` and `interruptline.test`'s `fire()` switch to it. `init_lines()` refuses a chip with no eoi, since a line in the table is what tells arch the interrupt is ended. `ioapic_issue_eoi()`, the directed EOI, stays as it is, reached only through `issue_ack`; `XAPIC_CHIP` stays unbound.
- Why now: steps 4 and 6 build `handle_fasteoi` and `handle_edge`, whose order of mask, actions, unmask and EOI is the whole mechanism. That needs the EOI inside kernel/irq first, and a test entry that runs the flow without it, so the flows can be tested from thread context without telling a controller that an interrupt ended.
- Verified: format_cpp on every changed C++ file; clang-tidy on the eight touched sources, nothing on a changed line; the build and both boots, rel with both `pc-ps2: key` lines; the three kernel tests, `irq.test` with six new checks on a counting chip (39 in all).
- Module: `kernel-irq`.
- Commit: `refactor(irq): end each interrupt through its line's chip, and give the tests a flow without the EOI` (83e21d8c), with the review's fixup, which adds `interruptline.test`'s check that arch ends a vector no line has (43 checks).

### 4. Flows per line, selected per D8
- What: a line keeps its flow, `irq::Flow` (`Edge` or `FastEoi`), under L1. `Desc::init()` chooses it from the chip's `trigger_mode` as `init_lines()` binds the line, and `configure()` and `attach()` with a mode choose it again under L1 from the mode set or read. `handle_irq()` reads it under the same hold as the actions (without L1 on a Permanent line, whose flow never changes again) and runs `handle_edge()` or `handle_fasteoi()`. An interrupt on a line nobody is on masks the line through its chip and then ends the interrupt, on either flow; `init_lines()` refuses a chip that cannot mask. Lines with an action run as before: the actions, then the end of interrupt.
- Why now: step 6's Oneshot mask goes into `handle_fasteoi()` alone, so each line must already know which flow it runs, and the mask needs its first writer inside kernel/irq: the line nobody is on, which no object masks.
- Verified: format_cpp on every changed C++ file; clang-tidy on the four touched sources, nothing in an irq header or the test; the build and both boots, rel with both `pc-ps2: key` lines; the three kernel tests, `irq.test` with eleven new checks on lines of the test's own (50 in all).
- Module: `kernel-irq`.
- Commit: `feat(irq): give each line a flow chosen from how its pin signals, and mask a line nobody is on` (ae092d8e), with the review's fixup, which makes the masking check see that the mask comes before the end of interrupt.

### 5. Test-only mask read-back (D10 (b))
- What: `irq::Chip` gains an optional `is_masked`, which the IO-APIC chip answers from the mask bit of the pin's redirection entry under LI (`IoApic::is_masked()`); `Desc::is_masked()` and `DescTable::is_masked()` reach it without L1, as `trigger_mode()` does, and `irq::is_masked_for_test()` is the one public entry. `interruptline.test` reads the free line's pin back around everything an object does: a level object's line unmasked once created, masked by an interrupt and kept masked through the wait that takes it, unmasked by the next wait (unbound) or by the ack that finds nothing to send (bound), masked again by the next interrupt, kept masked by an ack that sends the interrupts since, and masked by an interrupt raised through the interrupt entry; an edge object's line never masked. `irq.test` reads the idle line's pin back after its mask, unmask and mask, and checks the read's NoResource and NotSupported answers. Closes `docs/repairs/261005/object.md` item 1 with its option (b).
- Why now: steps 6 and 7 move a level line's mask from the interrupt object into the line's flow, and the user-visible promise of D1 (b) is that the mask behaves as it does today. The objects' packets and wakeups look the same whether the pin is masked or not, since the tests' interrupts go around the IO-APIC, so only a read of the pin can show that the move kept the behaviour: the same checks pass on today's objects now, and must pass again after step 7.
- Verified: format_cpp on every changed C++ file; clang-tidy on the six touched sources, nothing on a changed line; the build and both boots, rel with both `pc-ps2: key` lines; the three kernel tests, `irq.test` with two new checks (52 in all), `interruptline.test` with twelve (55 in all); and one run of `interruptline.test` against a mutant that gives the mask flags to edge objects instead of level ones, which failed the six level masking checks and the edge check.
- Module: `kernel-irq`.
- Commit: `test(irq): read a line's mask back from its controller, and check what level and edge objects leave there` (b5d6eb60), with the review's fixup, which adds the checks that an unbound wait taking the interrupt since leaves a level line masked and that a destroy masks the line it lets go, and polls for the raised interrupt's mask as for its packet.

### 6. ONESHOT in kernel/irq
- What: `IrqReturn` gains `WakeThread` and `IrqFlags` gains `Oneshot`. An action that says Oneshot makes its line Oneshot for as long as it is on it; `attach()` refuses Oneshot together with Permanent or Shared (InvArgs), and on a line whose chip cannot mask and unmask (NotSupported). `Desc` keeps one held bit under L1. `handle_fasteoi()` on a Oneshot line masks the line before the action, sets held if the action answered WakeThread, unmasks the line unless it is held, and then ends the interrupt; `handle_edge()` ignores Oneshot. The new `irq::finalize_oneshot(VIrqNum, Action const &)` takes L1, does nothing if the action is no longer on that line or the line is not Oneshot, and otherwise clears held and unmasks. `detach()` clears held and, when the last action goes, clears Oneshot and masks the line. WakeThread outranks Handled in a dispatch's answer and is not counted as spurious. Nothing attaches with Oneshot yet: the interrupt objects keep their own mask until step 7.
- Why now: step 7 switches the objects to Oneshot, and a level object's user-visible promise (masked from delivery to its ack) then rests on this mechanism alone. It has to exist, and be tested on a real pin, before any object depends on it.
- Verified: format_cpp on every changed C++ file; clang-tidy on `desc.cpp`, `desc_table.cpp`, `mod.cpp` and `irq_test.cpp`, nothing in an irq header or on a changed line; the build and both boots, rel with both `pc-ps2: key` lines; the three kernel tests, `irq.test` with sixteen new checks (68 in all), eleven of them on the idle line set level and read back from its IO-APIC entry; and two runs of `irq.test` against mutants, which failed exactly the checks each mutation targets.
- Module: `kernel-irq`.
- Commit: `feat(irq): hold a Oneshot level line masked from the interrupt its action wakes a thread for until finalize_oneshot()` (821960f9), with the review's fixup, which adds the check that a finalize on a line that is not Oneshot does nothing, counts Oneshot among what a Permanent line's flow reads without L1, and says that `irq::unmask()` does not ask whether the line is held.

### 7. Switch the objects (D1 (b))
- What: `InterruptEventDispatcher`'s action says Oneshot and its handler always answers WakeThread, Destroyed included, so a level line stays held masked until the detach. `mask_interrupt()` is empty. `unmask_interrupt()` is `irq::finalize_oneshot(m_line, m_action)`, and the state machine calls it only once the debt is cleared and only with L2 dropped: `ack()` on the path that moves to Idle, after its locked scope; and `wait()` after `begin_wait()`, which now returns a `WaitStart` whose `Debt::Cleared` marks a wait in NeedAck that found nothing pending, before the test hook and the park. MaskPostwait, UnmaskPrewait and `set_level_triggered()` are gone, and with them the only reason `m_flags` was guarded: it is `Flags const`. `create()` no longer reads the mode attach answers. `interruptline.test`'s first dispatch expects WakeThread. `docs/repairs/261007/kernel-irq.md` item 1 records the finalize an edge object's ack now costs.
- Why now: step 6 built ONESHOT in kernel/irq and tested it on a real pin, so the objects can now stop writing the mask themselves. Until they do, a level line's mask has two writers, one of them without L1; and step 8 cannot make `mask()` and `unmask()` take L1 while a handler, running under L1, still masks its own line through `mask()`.
- Verified: format_cpp on every changed C++ file; clang-tidy on `interrupt_dispatcher.cpp`, `interrupt_event_dispatcher.cpp`, `interrupt_line_test.cpp` and `desc.cpp`, nothing in a touched file; the build and both boots, rel with both `pc-ps2: key` lines; the three kernel tests with the same counts as before (69, 58, 32), step 5's level and edge mask checks unchanged; and one run of `interruptline.test` against a mutant whose handler answers Handled, which failed exactly the WakeThread check and the seven level mask checks.
- Module: `kernel-irq`.
- Commit: `refactor(object): interrupt objects attach Oneshot and leave a level line's mask to its flow` (616dba73), with the review's fixup, which corrects comments only (two that named the design after its model, `create()`'s stale reason for not destroying on failure, and a test comment that had the handler mask after queueing) and records in the change record a second mutant, which failed exactly the ack and wait debt checks, and that no test plays an interrupt between a destroy's Destroyed and its detach; and a second fixup, which declares the object's four virtuals `TA_EXCLUDES(m_spinlock)`, so that a finalize under L2 fails the build.

### 8. mask and unmask take L1
- What: `Desc::mask()` and `Desc::unmask()`, behind `irq::mask()` and `irq::unmask()`, take L1 around the chip's operation, so every write of a line's mask is made under the line's lock; `unmask()` reads held in the same hold and leaves a held line masked, answering Ok, since `finalize_oneshot()` unmasks it once the thread is done. Both are declared `TA_EXCLUDES(m_lock)`. They inherit the finalize's context rules: never from a handler of the same line, never with a lock the line's handler takes. `irq.test` checks that an unmask of the held idle line leaves its pin masked, and that each call, made while a dispatch on another CPU holds the line, returns only once that dispatch's handler has finished.
- Why now: step 7 removed the last handler that masked its own line, the reason mask and unmask took no L1. Until this step `irq::unmask()` was the one unmask that neither cleared held under L1 nor checked it, so the invariant a Oneshot level line rests on, masked while held, had a hole; an interrupt object's `create()`, which starts its line with it, could open a line an interrupt had held since the attach.
- Verified: format_cpp on every changed C++ file; clang-tidy on `desc.cpp`, `desc_table.cpp`, `mod.cpp`, `irq_test.cpp` and `interrupt_event_dispatcher.cpp`, nothing in a touched file; the build and both boots, rel with both `pc-ps2: key` lines; the three kernel tests, `irq.test` with three new checks (72 in all), `interruptline.test` 58 and `interrupt.test` 32 as before; a compile-only mutant that calls `unmask()` from inside the Oneshot flow fails the build, and a runtime mutant without the lock and the held check failed exactly the three new checks.
- Module: `kernel-irq`.
- Commit: `feat(irq): mask and unmask take the line's lock, and unmask leaves a held line masked`.

## Files
- `.agents/skills/ours-kernel-design/references/task.md`, `.agents/skills/ours-kernel-design/SKILL.md` — step 1, the recorded exception
- `.agents/tasks/irq-genirq/DESIGN.md` — step 1, Accepted
- `docs/changes/261005/kernel-irq.md` — every step's change record
- `kernel/include/ours/irq/{desc,desc_table,action,chip,mod,types}.hpp`, `kernel/kernel/irq/{desc,desc_table,mod}.cpp`, `kernel/kernel/irq/BUILD.gn` — step 2, the renamed types and files
- `kernel/object/interrupt_{event_,}dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_{event_,}dispatcher.hpp`, `kernel/arch/x86/apic/{io_apic,local_apic}.cpp`, `kernel/arch/x86/kernel/include/ours/arch/apic.hpp`, `kernel/arch/x86/kernel/{faults.cpp,irq/init.cpp,time/time.cpp}`, `kernel/commands/irq_test.cpp` — step 2, the callers
- `docs/repairs/261004/kernel-irq.md`, `docs/repairs/261003/x86-apic.md`, `docs/repairs/261004/x86-clock.md` — step 2, open items that named the old types or called an action an observer
- `kernel/include/ours/irq/{types,chip,desc,desc_table,mod}.hpp`, `kernel/kernel/irq/{desc,desc_table,mod}.cpp` — step 3, the flow's EOI and the two dispatch entries
- `kernel/arch/x86/apic/io_apic.cpp`, `kernel/arch/x86/kernel/faults.cpp`, `kernel/platform/pc/interrupt.cpp`, `kernel/platform/include/ours/platform/interrupt.hpp` — step 3, the chip's eoi and arch's EOI for a vector with no line
- `kernel/commands/irq_test.cpp`, `kernel/commands/interrupt_line_test.cpp` — step 3, the test entry and the counting-chip checks
- `docs/repairs/261003/x86-apic.md`, `docs/repairs/261004/kernel-irq.md` — step 3, items 1 and 2 off the path, item 3 naming the test entry
- `kernel/include/ours/irq/{types,chip,desc,mod}.hpp`, `kernel/kernel/irq/{desc,desc_table}.cpp` — step 4, the flow per line, its selection and the mask of a line nobody is on
- `kernel/commands/irq_test.cpp` — step 4, the pin chip and the flow checks
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/ioapic.hpp`, `kernel/arch/x86/apic/io_apic.cpp` — step 5, the mask bit read from a pin's redirection entry, as the IO-APIC chip's `is_masked`
- `kernel/include/ours/irq/{chip,desc,desc_table,mod}.hpp`, `kernel/kernel/irq/{desc,desc_table,mod}.cpp` — step 5, `Chip::is_masked` and `irq::is_masked_for_test()`
- `kernel/commands/interrupt_line_test.cpp`, `kernel/commands/irq_test.cpp` — step 5, the level and edge mask checks, and the idle line's read-back
- `docs/repairs/261005/object.md` — step 5, item 1 closed
- `kernel/include/ours/irq/{types,action,chip,desc,desc_table,mod}.hpp`, `kernel/kernel/irq/{desc,desc_table,mod}.cpp` — step 6, `WakeThread`, `Oneshot`, held, the Oneshot flow, `finalize_oneshot()` and detach's clearing and masking
- `kernel/commands/irq_test.cpp` — step 6, the Oneshot checks on the idle line and on lines of the test's own
- `.agents/skills/ours-kernel-design/references/task.md`, `.agents/tasks/irq-genirq/DESIGN.md` — step 6, Oneshot never shares, and the Staging's revision
- `kernel/object/interrupt_{event_,}dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_{event_,}dispatcher.hpp` — step 7, Oneshot, WakeThread, the empty mask and the finalize as `unmask_interrupt()`
- `kernel/include/ours/irq/desc.hpp`, `kernel/kernel/irq/desc.cpp`, `kernel/commands/interrupt_line_test.cpp` — step 7, comments that named the object as a masking handler, and the WakeThread check
- `.agents/skills/ours-kernel-design/references/task.md` — step 7, the objects write no mask
- `docs/repairs/261007/kernel-irq.md` — step 7, the finalize on edge acks
- `kernel/include/ours/irq/{desc,mod,chip}.hpp`, `kernel/kernel/irq/desc.cpp` — step 8, mask and unmask under L1, held read by unmask, and the contract that follows
- `kernel/arch/x86/kernel/include/ours/arch/apic.hpp`, `kernel/object/interrupt_event_dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp` — step 8, comments on who writes a pin's mask and on `create()`'s start unmask
- `kernel/commands/irq_test.cpp` — step 8, the held unmask check and the two lock checks
- `.agents/skills/ours-kernel-design/references/task.md` — step 8, every write of a line's mask under L1
- `.agents/tasks/irq-genirq/RESULT.md` — after step 8, the task's result
- Later steps add their files here as they start.

## Test plan
- Every step: boot_check prints `dbg PASS` and `rel PASS` with `6/6 required`, and kshell_run prints `irq.test: ok`, `interruptline.test: ok` and `interrupt.test: ok`.
- The checks the steps add are DESIGN.md's Testing and verification, each with the step that brings its mechanism: in `irq.test`, flow selection and a no-action line masked (step 4), then held set by Oneshot and cleared by finalize, detach clearing held, Permanent with Oneshot refused and WakeThread not counted as spurious (step 6); in `interruptline.test`, the level and edge mask read-backs (step 5, passing again after step 7).
