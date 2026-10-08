# lockup-watchdog: a CPU that stops ticking, or a machine that stops altogether, reports every CPU's state once and panics, on both images, with the threshold set on data

**Date**: 2026-10-08

## Delivered
The DESIGN's staging steps 1 to 4, one commit each on `feat/lockup-watchdog`:

- Step 1, `09f3c630`, `feat(lockup): dump every CPU's state on an NMI nothing in the kernel sent`. The library `kernel/lib/lockup_detector`: the report claim, a context slot per CPU, the target that fills its slot and parks in its NMI, the reporter that waits for the others and prints one block per CPU in panic mode (thread, pc as an image offset, interrupts on or off, spin locks held, registers, a frame-pointer backtrace), then panics. The NMI vector hands every NMI to `lockup_detector::on_nmi()`; `arch_mp_send_nmi()`; LINT1 routed to NMI on the boot CPU; `lockup nmi`.
- Step 2, `2867a8eb`, `feat(lockup): a CPU whose tick stops is found by the others and reported`. A per-CPU heartbeat moved first in every tick; once a second of its own ticks every CPU checks every other on private notes (`HeartbeatNote`, host-tested); a target unchanged for `kernel.lockup.threshold` checks (default 10, 0 off) is reported, its block last; `lockup spin <cpu> [<ms>]`.
- Step 3, `3a9866a1`, `feat(lockup): the boot scripts send QEMU's nmi on a silent console and judge the lockup reports`. `boot_check.py` and `kshell_run.py` send QEMU's monitor `nmi` after 15 s of silence and keep the report; `kshell_run.py` judges the lockup commands; `lockup spin all`; the ours-verify skill's "Reading a lockup report".
- Step 4, the commit that adds this document, `feat(lockup): lockup status shows each checker's notes, and six boots keep the threshold at 10`. A note's high-water marks (most checks in a row without a tick, fewest ticks between two checks); `lockup status`, each CPU's notes copied on that CPU in an IPI; the threshold measured and kept at 10.

Steps 5 and 6 are optional and not done (Not done, below).

## Verification
- Every step: `boot_check.py` in the worktree, both images PASS, rel's `pc-ps2: key a` and `pc-ps2: key A`, no `lockup:` line and no `nmi` sent. Step 4's final run: dbg PASS (shell finished at 12.9 s, 35 passed), rel PASS (4.1 s, 33 passed, 6/6 required), host-tests PASS (143).
- Host test: `//kernel/lib/lockup_detector/test:heartbeat_note`, 9 tests, in `//:default`.
- The report, on dbg through `kshell_run.py` (details in PLAN.md and the change records `docs/changes/261007/lockup-detector.md`, `docs/changes/261008/lockup-detector.md`):
  - `lockup nmi`: one block per CPU, once, then one panic; with `--reboot`, the crash record on the next boot.
  - `lockup spin 2`: reported by the checker 10.07 s to 10.31 s after the spin began, cpu2 last at `spin_routine+0x32` with interrupts off; `lockup spin 2 8000` stays quiet.
  - `lockup spin all`: reported only through the script's `nmi`, every block with interrupts off.
  - rel, which has no kernel shell, through temporary code: the checker's report 10.72 s after a spin, and the silence `nmi`'s report.
- The NMI stack: the reporter used at most 3896 of its 16384 bytes (dbg), a target 1124.
- The threshold, step 4, six boots: no CPU went even one check without a tick, alone or with a second QEMU on the host.

| Boot | Image | On the host at the same time | Most checks in a row without a tick | Fewest ticks between two checks |
|---|---|---|---|---|
| 1 | dbg | nothing | 0 | 72 (cpu0) |
| 2 | rel | nothing | 0 | 99 |
| 3 | dbg | boot 4 | 0 | 56 (cpu0) |
| 4 | rel | boot 3 | 0 | 99 |
| 5 | dbg | boot 6 | 0 | 75 (cpu0) |
| 6 | dbg | boot 5 | 0 | 72 (cpu0) |

- Not measured: catching the real hang, which has not recurred; an NMI landing between the two ICR writes (the DESIGN's accepted hazard); a host loaded by a build as well as a second QEMU; real hardware.

## Deviations from the design
Each step's revisions are in DESIGN.md, Staging, under "Revised", each keeping the decisions D1-D14 as taken. The ones a reader of the code meets first:

- The namespace is `lockup_detector`, the library's directory name, not `lockup`.
- Arch supplies a third hook, `arch_dump_registers()`, since the register names are arch's; `arch::IrqFrame` gained `irqs_enabled()`.
- The context slots are a plain array indexed by CPU, not `CPU_LOCAL`, so an NMI before `CpuLocal::init()` still has one.
- A target already in a panic of its own fills its slot and returns to that panic instead of parking (D7 holds for every other target); `panic.cpp`'s flag became `PANIC_CPU`, with `this_cpu_in_panic()` beside `panic_in_progress()`.
- The pure check is `HeartbeatNote`, the one part of the library that builds on the host.
- `lockup spin all` came with step 3, every thread made and pinned before any is let go.
- Step 4: a note also keeps the fewest ticks between two checks, since a healthy boot scores 0 on the count alone; `lockup status` prints every checker's notes and copies them on their own CPU rather than reading them across CPUs; the loaded case was a second QEMU boot at the same time, with no build alongside.

## Found along the way
- `docs/repairs/261008/kernel-lib-logz4.md`: on the dbg image the boot CPU loses 10% to 16% of its ticks, up to 44% in one second, probably waiting for the console's unfair lock with interrupts off. Found by `lockup status`; not chased.
- `docs/repairs/261007/kernel-smp.md`, item 1, the hang that started this task: still open, and now says how the next one reports itself.

## Not done
- Step 5 (D12 (b)): record the lock a spinning CPU waits on, so that the report names its holder. In DESIGN.md, Staging.
- Step 6 (D10 (b)): `.eh_frame` unwinding, which would recover a leaf function's caller. In DESIGN.md, Staging.
- The DESIGN's open questions that remain: whether QEMU runs the vCPUs round-robin or one thread each; whether a hunt should keep QEMU alive after the report (D8 (c)); what the checker does once tickless idle stops idle CPUs' ticks; whether `walk_frame_pointers()` moves to `kernel/lib/unwind`.
- The predictions and the D2, D6 and D7 concurrency arguments that the working rules ask the user for: the user skipped them on 2026-10-07 ("直接上"). DESIGN.md, under Decisions, names two places to start if they are taken up.
