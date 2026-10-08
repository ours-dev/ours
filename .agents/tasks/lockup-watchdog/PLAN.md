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
2. **Heartbeat and checker.** Not started. The heartbeat, the per-checker snapshots, `kernel.lockup.threshold`, the panic stand-down and `lockup spin <cpu> [<ms>]`, with the check's host test.
3. **Scripts.** Not started. `kshell_run.py` and `boot_check.py` learn `lockup` and send `nmi` on silence.
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
- `kernel/kernel/time/tick_sched.cpp`, `kernel/include/ours/boot/options.hpp`, `kernel/top/boot_options.cpp` — heartbeat and threshold (step 2).
- `scripts/kshell_run.py`, `.agents/skills/ours-verify/scripts/boot_check.py` — step 3.

## Test plan
- Every step: `mkdir -p out/tmp && TMPDIR=$PWD/out/tmp python3 .agents/skills/ours-verify/scripts/boot_check.py` in the worktree: dbg and rel PASS, rel.log has `pc-ps2: key a` and `pc-ps2: key A`, and no `lockup:` line in either log.
- Step 1: on dbg, `lockup nmi` prints `lockup: dump asked for by an NMI on cpu<N>; each CPU's state follows`, one block per online CPU (thread, pc offset, IF, held spin locks, five register lines, one backtrace line), then `OURS KERNEL PANIC` once, and with `REBOOT=1` the next boot logs `crashlog: | lockup: dump asked for by an NMI on cpu<N>`. QEMU's monitor `nmi` gives the same report from cpu0, on dbg and on rel.
- Step 2: `lockup spin 2` reports once within the threshold plus about 2 s, naming cpu2 with IF off and its pc in the spin; `lockup spin 2 3000` stays quiet; the check's host test passes.
- Step 3: `lockup spin all` is caught only through the script's `nmi`.
