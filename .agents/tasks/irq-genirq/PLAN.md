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
- Later steps add their files here as they start.

## Test plan
- Every step: boot_check prints `dbg PASS` and `rel PASS` with `6/6 required`, and kshell_run prints `irq.test: ok`, `interruptline.test: ok` and `interrupt.test: ok`.
- The checks the steps add are DESIGN.md's Testing and verification, each with the step that brings its mechanism: in `irq.test`, flow selection and a no-action line masked (step 4), then held set by Oneshot and cleared by finalize, detach clearing held, Permanent with Oneshot refused and WakeThread not counted as spurious (step 6); in `interruptline.test`, the level and edge mask read-backs (step 5, passing again after step 7).
