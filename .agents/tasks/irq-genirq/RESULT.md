# irq-genirq: kernel/irq rebuilt around line descriptors and flows, with ONESHOT owned by the line and the interrupt objects as its threaded handlers

**Date**: 2026-10-07

## Delivered
All eight steps of DESIGN.md's Staging, one mechanism per step, each with its entry in `docs/changes/261005/kernel-irq.md` (steps 1 to 6) or `docs/changes/261007/kernel-irq.md` (steps 7 and 8). Hashes are those on `baseline/20260824` for steps 1 to 6, and on `irq/genirq` for steps 7 and 8, which land from there.

- Step 1, the recorded exception: `ours-kernel-design`'s `references/task.md` says kernel/irq follows genirq, and DESIGN.md is Accepted. `7444a31e`, with the review's fixup `3d2a9ee9`.
- Step 2, the renames per D6: `irq::Desc`, `irq::Action`, `irq::Chip`, `irq::Data`, `irq::DescTable`. `7bb24bb2`, fixup `46169ebd`.
- Step 3, the EOI through the chip: the line's flow ends each interrupt through its chip, arch only a vector with no line, and `irq::handle_irq_for_test()` runs the flow without the EOI. `fa506351`, fixup `d41a9ae6`.
- Step 4, a flow per line (D8): `handle_edge` or `handle_fasteoi`, chosen from the pin's trigger mode when the table is built and again by `configure()` and attach; a line nobody is on is masked. `d9175755`, fixup `099c82c1`.
- Step 5, the test-only mask read-back (D10 (b)): `Chip::is_masked` and `irq::is_masked_for_test()`, and `interruptline.test`'s level and edge mask checks against the objects as they were. `96bdf026`, fixup `21ace3ff`.
- Step 6, ONESHOT in kernel/irq: `WakeThread`, `IrqFlags::Oneshot`, the held bit under L1, the Oneshot level flow, `irq::finalize_oneshot()`, detach clearing held. `821960f9`, fixup `bf6efd74`.
- Alongside, outside the Staging, under the user's style rules of 2026-10-06: the end of interrupt through an `EoiGuard` held across the flow (`942ab2b7`), and held as a bit of the line's flag word rather than a bool (`44d90835`).
- Step 7, the objects switched (D1 (b)): `InterruptEventDispatcher` attaches Oneshot and always answers WakeThread, `mask_interrupt()` is empty, `unmask_interrupt()` is the finalize, called outside L2 only when the debt is cleared; MaskPostwait, UnmaskPrewait and `set_level_triggered()` are gone. `616dba73`, with the review's fixups `8ae31240` (comments and the record) and `f1634c25` (`TA_EXCLUDES(m_spinlock)` on the four virtuals).
- Step 8, mask and unmask under L1: `irq::mask()` and `irq::unmask()` take the line's lock like every other write of a line's mask, and `unmask()` leaves a held line masked, answering Ok. `cd23f293`.

## Verification
Every step built `out/bringup.x86`, booted both images under `boot_check.py` (dbg and rel PASS, rel with `pc-ps2: key a` and `pc-ps2: key A`), and ran `irq.test`, `interruptline.test` and `interrupt.test` on dbg, all ok. The kernel tests' check counts as the steps added to them:

| After step | irq.test | interruptline.test | interrupt.test |
|---|---|---|---|
| 1 (the baseline) | 33 | 41 | 32 |
| 2 | 33 | 41 | 32 |
| 3 | 39 | 43 | 32 |
| 4 | 50 | 43 | 32 |
| 5 | 52 | 58 | 32 |
| 6 | 69 | 58 | 32 |
| 7 | 69 | 58 | 32 |
| 8 | 72 | 58 | 32 |

Each row is the step with its review fixups, as its change record gives the counts; no step changed interrupt.test.

Step 8 on the tree as committed:
- `boot_check.py`: dbg PASS (shell finished at 48.0s; 33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required); rel.log has `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A`.
- `kshell_run.py --out out/bringup.x86 irq.test interruptline.test interrupt.test`: irq.test ok (72), interruptline.test ok (58), interrupt.test ok (32).
- format_cpp clean on the eight changed C++ files; clang-tidy on `desc.cpp`, `desc_table.cpp`, `mod.cpp`, `irq_test.cpp` and `interrupt_event_dispatcher.cpp` reports nothing in a touched file.
- Mutants: a compile-only one that calls `unmask()` from inside the Oneshot flow fails the build on the thread-safety analysis; a runtime one with `mask()` and `unmask()` taking no L1 and ignoring held failed exactly the three new `irq.test` checks of 72.
- Boots for step 8: two boot_checks of both images and three kshell_runs of dbg (the three tests twice, `irq.test` once against the mutant).

Mutants across the task, each failing exactly the checks it aimed at: step 5 (mask flags given to edge objects instead of level ones), step 6 (two, on the Oneshot flow), step 7 (the handler answering Handled; the ack and wait finalizing while still owed; a compile-only finalize under L2), step 8 (above).

Not measured: any cost. The time L1 is held across a dispatch, the finalize every edge ack now pays (`docs/repairs/261007/kernel-irq.md` item 1), and the time a mask or unmask waits for a handler are all unmeasured. Not verified: that the LAPIC EOI clears a level pin's Remote IRR, since no real level device fires on q35 (DESIGN Open questions); the level path is checked through the pin's mask bit read back from its IO-APIC entry, with interrupts played by `handle_irq_for_test()` or raised by IPI. Lock order rests on the written argument and the thread-safety annotations: lockdep cannot see L1 or LI (`docs/repairs/261004/kernel-irq.md` item 2).

## Deviations from the design
- Step 6 revised the Staging, as DESIGN.md records: attach refuses Oneshot with Shared as well as with Permanent (InvArgs), and on a line whose chip cannot mask and unmask (NotSupported); a finalize on a line that is not Oneshot does nothing; the Oneshot flow unmasks before the EOI.
- Step 7: `begin_wait()` returns a `WaitStart` carrying `Debt::Cleared`, a shape the DESIGN did not name, and the review declared the object's four virtuals `TA_EXCLUDES(m_spinlock)`, so a finalize under L2 fails the build.
- Step 8: the DESIGN says unmask leaves a held line masked but not what it answers; it answers Ok, the reasons and the rejected refusal in the step's change record. `Desc::mask()` and `Desc::unmask()` are declared `TA_EXCLUDES(m_lock)`, and `irq.test` gained a check that each waits for a handler running on another CPU, which the DESIGN's test list did not ask for.
- `irq::finalize_oneshot()` takes `Action const &`, where the DESIGN's API sketch has `Action &`: it reads the action, never writes it.
- The PLAN's commit hashes for steps 1 to 5 are those of the lane they were made in; `baseline/20260824` carries the same changes under the hashes in Delivered.

## Found along the way
- `docs/repairs/261004/kernel-irq.md` item 2's Symptom still lists "an interrupt object's mask, which takes LI inside L2" among the nestings lockdep would misreport. Step 7 removed that mask, and since step 8 `irq::mask()` takes LI inside L1, as `configure()` does. The item's substance stands; its example is stale. Not edited here.
- Step 7's change record lists, under Deliberately not done, the `create()` window that step 8 closes. Change records are append-only, so step 8's entry says it instead.
- A mask is not a disable: an interrupt already on its way when `irq::mask()` lands can still have a Oneshot line's flow unmask the line, and any later unmask opens it. No caller masks a Oneshot line today, so this is no repair item; it is what D5's deferred depth would address, and step 8's change record says so.

## Not done
- DESIGN.md's Deferred list, each a task of its own: disable depth, lazy disable and resend (D5); stuck-line disabling; irq_domain and vector allocation, with MSI (D4; `domain.hpp` stays as scaffolding); affinity (`irq::set_affinity()` is still a stub); thread-context dispatch with preemption on (D9 (c)), `docs/repairs/261004/kernel-irq.md` item 3.
- Performance work, the user's to decide item by item: skipping the finalize on edge acks (`docs/repairs/261007/kernel-irq.md` item 1), a shorter L1 hold, vector matrix tuning.
- DESIGN.md's Non-goals: a software Masked bit, a per-action oneshot bitmap, in-kernel interrupt threads (D3), shared lines for user objects.
- Lockdep classes for L1 and LI: `docs/repairs/261004/kernel-irq.md` item 2.
- Tests no step added: an interrupt between `destroy()` setting Destroyed and its detach (step 7's record), and one between an object's attach and its start unmask (step 8's record); both need a test hook inside the object.
- DESIGN.md's Open questions stay open: the finalize exercise and interleaving B, both the user's; whether MSI gets a one-shot-safe chip or an object that masks inside its device; whether the IO-APIC drops an edge on a masked pin; whether the LAPIC EOI clears a level pin's Remote IRR on ours's setup.
- The user skipped the prediction for D1 and the concurrency argument for D2 on 2026-10-05, and no core function of this milestone has yet been picked for the user to write by hand (`docs/authored.md`).
