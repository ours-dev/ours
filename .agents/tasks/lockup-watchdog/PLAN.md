# lockup-watchdog: land the lockup watchdog in the DESIGN's six staged steps, one commit each

**Date**: 2026-10-07
**Status**: Accepted

## Request
On 2026-10-07 the user accepted `.agents/tasks/lockup-watchdog/DESIGN.md` with "直接上": every decision D1-D14 as recommended, with the predictions and the D2/D6/D7 arguments skipped. The work is the DESIGN's Staging, one step per commit on branch `feat/lockup-watchdog` (worktree `.worktrees/feat-lockup-watchdog`), each step built and booted with `ours-verify` before its commit. The scope ends where the Staging ends: steps 5 and 6 are optional, and nothing outside the DESIGN is added.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1-D14 | As listed in DESIGN.md | As listed there | Skipped | As recommended, 2026-10-07, 直接上; the user skipped the prediction and the D2/D6/D7 arguments |

## Steps
1. **NMI dump on demand.** Done on 2026-10-07, commit `feat(lockup): dump every CPU's state on an NMI nothing in the kernel sent`.
   - What: `kernel/lib/lockup_detector` (the claim, the request mask, one context slot per CPU, the target's capture and park, the reporter's wait and print in panic mode, frame-pointer backtraces as image offsets); the NMI vector hands every NMI to `lockup_detector::on_nmi()`; `arch_mp_send_nmi()` through the bounded `try_send_ipi()`; LINT1 routed to NMI on the boot CPU and masked on the others; `panic_in_progress()`, and `this_cpu_in_panic()` so that a target in a panic of its own goes back to it instead of parking (added in review); `lockup nmi` in `kernel/commands/lockup.cpp`.
   - Why first: everything after it reports through it, and on its own it already lets a silent boot be dumped from QEMU's monitor.
   - Verified: `boot_check.py` both images PASS; `lockup nmi` on dbg prints cpu0-cpu3 once, then the panic, and the next boot reports the crash record; the monitor's `nmi` gives the same report on dbg and rel. The open questions on QEMU's `nmi` and the NMI stack depth are answered in DESIGN.md. Deviations are in DESIGN.md, Staging step 1, "Revised".
2. **Heartbeat and checker.** Done on 2026-10-07, commit `feat(lockup): a CPU whose tick stops is found by the others and reported`.
   - What: a per-CPU heartbeat moved first thing in every `TickSched::on_tick()`; once a second of its own ticks every CPU checks every other online CPU on notes of its own (`HeartbeatNote`, host-tested); a note at `kernel.lockup.threshold` checks (default 10, 0 off) claims the report, which gathers every CPU through the step-1 NMIs and prints the stuck CPU last, then panics `lockup: cpu<N> took no tick for <T> s`; the checks start at InitLevel::Platform and stand down during a panic and once the report is claimed; each CPU's block carries its heartbeat (`ticks taken`, added in review); `lockup spin <cpu> [<ms>]`.
   - Why now: step 1's dump needs something outside to ask for it; this is the part that asks from inside, for the hang in which some CPU still ticks.
   - Verified: `boot_check.py` both images PASS, armed at 10 s, no `lockup:` line; on dbg `lockup spin 2 8000` returned and nothing was reported in the 15 s after it, then `lockup spin 2` in the same boot was reported 10.07 s after it began, cpu2 at `spin_routine+0x32` with interrupts off, printed last, one panic; on rel, through temporary code, the same report 10.72 s after the spin began. Reviewed on 2026-10-07: after the slot's heartbeat was added, `boot_check.py` both images PASS again and, on dbg, `lockup spin 2 8000` stayed quiet and `lockup spin 2` was reported 10.31 s after it was typed, cpu2 at `spin_routine+0x32` with interrupts off, printed last. Deviations are in DESIGN.md, Staging step 2, "Revised".
3. **Scripts.** Done on 2026-10-07, commit `feat(lockup): the boot scripts send QEMU's nmi on a silent console and judge the lockup reports`.
   - What: `kshell_run.py` treats `lockup nmi`, `lockup spin <cpu>` and `lockup spin all` like `crash` and checks the report each leads to (which path reported it, one block per CPU, interrupts off where the command says, one panic); `lockup spin <cpu> <ms>` ends at its own line; `boot_check.py` and `kshell_run.py` send QEMU's monitor `nmi` after 15 s of silence (from the kernel's first line, before the end marker, and on dbg for the whole `kshell_run.py` run), wait for the report, hold a timeout off while the console is already quiet, and name the report in the verdict; `lockup spin all`; the ours-verify skill says what a lockup report looks like and how to read its offsets with `llvm-objdump`.
   - Why now: step 2 left the every-CPU hang to an `nmi` someone had to type; every boot the sessions run is now a witness to it.
   - Verified: `lockup spin all` on dbg reported only through the script's `nmi` (four blocks, interrupts off, `spin_routine+0x32`), also with a 5 s command timeout held off; `vmosnapshot.test`, `lockup spin 2 3000` and `lockup spin 2` in one dbg boot, with no `nmi`; `lockup nmi` with `--reboot` and the crash record on the next boot; `boot_check.py` with a temporary all-CPU stall at boot gave both images' reports through the `nmi`, also past a 12 s timeout; the final `boot_check.py` both images PASS with no `lockup:` line and no `nmi` sent. 12 QEMU instances. Reviewed on 2026-10-07: `kshell_run.py` no longer ends a wait that begins after an `nmi`'s report is over (`--reboot "lockup spin all"`'s wait for the crash record), and counts silence from the kernel's first line, not phys's; `kshell_run.py --reboot "lockup spin all"` ok with the crash record on the next boot, `boot_check.py` both images PASS with no `nmi` sent, 3 QEMU instances. Deviations are in DESIGN.md, Staging step 3, "Revised".
4. **Threshold data.** Not started. `lockup status`, and the margin measured under host load (concurrent boots need the user's leave).
5. **Optional: the spinning CPU's lock (D12 (b)).** Not started.
6. **Optional: `.eh_frame` unwinding (D10 (b)).** Not started.

## Files
- `kernel/lib/lockup_detector/` — new library (step 1): `BUILD.gn`, `README.md`, `include/lockup_detector/lockup_detector.hpp`, `src/report.cpp`.
- `kernel/commands/lockup.cpp`, `kernel/commands/BUILD.gn` — the `lockup` command (step 1; `spin` and `status` in steps 2 and 4).
- `kernel/arch/x86/kernel/faults.cpp` — the NMI case calls the detector; the register lines shared with the dump (step 1).
- `kernel/arch/x86/kernel/mp.cpp`, `kernel/arch/x86/kernel/include/ours/arch/mp.hpp` — `arch_mp_send_nmi()` (step 1).
- `kernel/arch/x86/apic/local_apic.cpp`, `kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp` — LINT1 (step 1).
- `kernel/arch/x86/lib/arch/include/arch/x86/interrupt.hpp` — `IrqFrame::irqs_enabled()` (step 1).
- `kernel/top/panic.cpp`, `system/public/ours/panic.hpp` — `panic_in_progress()` (step 1).
- `kernel/arch/x86/kernel/BUILD.gn` — the arch kernel links the detector (step 1).
- `kernel/kernel/time/tick_sched.cpp`, `kernel/kernel/time/BUILD.gn`, `kernel/include/ours/boot/options.hpp`, `kernel/top/boot_options.cpp` — heartbeat and threshold (step 2).
- `kernel/lib/lockup_detector/src/checker.cpp`, `src/report.hpp`, `src/checker.hpp`, `include/lockup_detector/heartbeat_note.hpp`, `test/` — the checker, its seams to the report and back, the pure check and its host test (step 2); `//BUILD.gn` runs the test in `//:default`.
- `scripts/kshell_run.py`, `.agents/skills/ours-verify/scripts/boot_check.py`, `.agents/skills/ours-verify/SKILL.md` — the silence `nmi`, the lockup commands' judging, how to read a report (step 3); `kernel/commands/lockup.cpp` gains `lockup spin all` (step 3).

## Test plan
- Every step: `mkdir -p out/tmp && TMPDIR=$PWD/out/tmp python3 .agents/skills/ours-verify/scripts/boot_check.py` in the worktree: dbg and rel PASS, rel.log has `pc-ps2: key a` and `pc-ps2: key A`, and no `lockup:` line in either log.
- Step 1: on dbg, `lockup nmi` prints `lockup: dump asked for by an NMI on cpu<N>; each CPU's state follows`, one block per online CPU (thread, pc offset, IF, held spin locks, five register lines, one backtrace line), then `OURS KERNEL PANIC` once, and with `REBOOT=1` the next boot logs `crashlog: | lockup: dump asked for by an NMI on cpu<N>`. QEMU's monitor `nmi` gives the same report from cpu0, on dbg and on rel.
- Step 2: `lockup spin 2` reports once within the threshold plus about 2 s, naming cpu2 with IF off and its pc in the spin, on dbg and (rel has no kernel shell) on rel through temporary code; `lockup spin 2 8000`, below the 10 s threshold, returns and nothing is reported in the 15 s after it; the check's host test passes; `lockup nmi` still gives its step-1 report.
- Step 3: `lockup spin all` is caught only through the script's `nmi`, with every block's interrupts off; a stall before the end marker makes `boot_check.py` send `nmi` and name the report, on both images; a normal `boot_check.py` and `kshell_run.py` run sends none.
