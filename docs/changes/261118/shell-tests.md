# shell-tests changes

## Title: A sampler check that caught nothing runs its round again, up to eight rounds (2026-11-18)

**Files**
- `system/app/shell/shell_tests.cpp` — `SAMPLER_MAX_ROUNDS`, `monotonic_ns()`, `rerun_while_silent()` and `report_rounds()` added; `check_sampler()` and `check_unmatched_ticks()` rerun a silent round, and their log lines say how many rounds and how much guest time it took

**Symptom**
GitHub CI run 44356406563 (baseline/20260824 at 6e58e3d3, the 3-core macos-26 runner booting dbg and rel side by side) failed rel with `shell: sampler checks FAILED`. The one failing line was `shell: sampler read 0 records`, after `sampler_start = Ok` and `sampler_stop = Ok`; every other sampler line passed. The same commit passed on `ci/probe` (run 44332760950) with `sampler read 32 records`, and passes locally.

**Cause**
Nothing in the kernel lost the samples: under QEMU's TCG no timer interrupt reached the guest during the round.

The objects:
- The session: one `time::Timer` per online CPU, armed by `Session::start()` through `sync_exec()` for one period (1 ms) from now, and re-armed by `on_sample_due()` with `forward(period)`.
- `ACTIVE`: the running session. `on_sample_due()` records only while `ACTIVE` names its session; `stop_locked()` clears `ACTIVE` first and then disarms every CPU's timer by IPI.
- Each CPU's local APIC timer, programmed one-shot by its `TimerQueue` for the earliest deadline.
- QEMU's main loop: one host thread. Under TCG every emulated APIC timer and the PIT is a QEMU timer on the virtual clock, and that thread runs their callbacks, which raise the interrupt on the vCPU. The vCPU threads run guest code on their own, and an IPI from one vCPU to another is delivered by the sending vCPU's thread, not by the main loop.

The events in the failing round:
- The shell starts the session; every CPU's sampler timer is due at T+1 ms.
- The shell's vCPU runs the 5000 `vmo_create`/`handle_close` calls, about 35 ms of guest time locally and longer on CI. Guest time is host time under TCG, so it passes whether or not the main loop runs.
- The main loop thread gets no host CPU for that whole stretch. No CPU's APIC timer callback runs, so no CPU takes a timer interrupt, and no sample is taken anywhere: not on the shell's CPU, not on the three idle ones.
- The shell calls `sampler_stop()`: `ACTIVE` is cleared, and the disarm IPIs go out vCPU to vCPU without needing the main loop. When the main loop does run, the overdue interrupts find `ACTIVE` cleared and record nothing, as they are meant to.
- The read finds an empty dump.

The same boot shows the main loop starved: APIC calibration logged "19 ticks spanned 48 tick periods, so about 29 ticks were lost", which is the PIT, driven by the same thread, coalescing 60% of its interrupts (the passing CI run lost 12 of 31). The failing run's other sampler numbers are low the same way: the kernel-only session counted 8 unmatched ticks on rel and 4 on dbg, against 18 and 17 in the passing run and about 50 locally, and the stress check gathered 15 records against 24.

So "0 records means a fault" was wrong. The check's comment reasoned in guest periods ("some four ticks on each CPU"), but a guest period only becomes an interrupt when the host thread that emulates the timer runs, and that thread is the one a loaded host starves. A fixed amount of guest work cannot guarantee a single timer interrupt.

**Change**
Before, one round decided the check:

```cpp
ok &= expect("shell: sampler_start = ", ours::sampler_start(handle), Status::Ok);
// ...
for (auto i = 0; i < 5'000; ++i) { /* vmo_create, handle_close */ }
ok &= expect("shell: sampler_stop = ", ours::sampler_stop(handle), Status::Ok);
// ...
ok &= records > 0;
```

After, the first round keeps its checks of start and stop, and a round that caught nothing is run again, up to eight rounds in all:

```cpp
auto rounds = u32{1};
while (rounds < SAMPLER_MAX_ROUNDS && !caught()) {
    // sampler_start, round(), sampler_stop
    ++rounds;
}
```

`caught` is a size query that finds more than the header for `check_sampler()`, and `num_unmatched > 0` for `check_unmatched_ticks()`, which has the same exposure: it fails at 0 the same way, and its count dropped to 4 on dbg in the failing run. Each restart is a fresh window, so one starved stretch of the main loop costs a round, not the check. A session that never samples still fails, after eight rounds (about 0.2 s on rel, idle). The log lines now carry the rounds and the guest time: `shell: sampler read 32 records in 1 round(s), 29 ms`.

Rejected:
- A timed sleep inside the session, which would have been airtight: the sleeping thread's wake-up timer and the already-due sampler timer sit in the same CPU's `TimerQueue`, so the interrupt that wakes the shell also runs the sampler's handler while `ACTIVE` is still set. But `object_wait_one` and `port_wait` take only 0 or `INSTANT_INFINITE`, and adding a timed wait is a userabi change, not a test fix.
- Looping until a guest-time deadline instead of a fixed count: a deadline is still guest time, which passes while the main loop is starved, so it narrows the window's odds without changing them; and it would cost every boot the whole deadline.
- Larger buffers or a shorter period: neither helps when no interrupt arrives at all.
- A guest-time budget for the reruns (the first draft: two seconds since the first start). Under load one round on dbg took 2282 ms, so that budget would have allowed no second round exactly when one is needed; a count of rounds does not depend on how slow a round is.

**Reference**
None, this project's own test. The emulation detail is QEMU's: `hw/intc/apic.c` arms the APIC timer with `timer_new_ns(QEMU_CLOCK_VIRTUAL, apic_timer, s)`, and without icount the virtual-clock timers run from `main_loop_wait()` via `qemu_clock_run_all_timers()`.

**Verification**
- `boot_check.py`: dbg PASS (46 passed, 5 skipped), rel PASS (44 passed, 8 skipped, 7/7 required), host tests 185 passed. rel: `sampler read 32 records in 1 round(s), 28 ms`, `kernel-only session counted 50 unmatched ticks in 1 round(s), 62 ms`; dbg: 32 records in 1 round (41 ms), 55 unmatched ticks in 1 round (68 ms).
- The rerun path, in throwaway builds: forcing `caught` false for the first two rounds gave rel `sampler read 32 records in 3 round(s), 79 ms` and `46 unmatched ticks in 3 round(s), 177 ms`; never consulting `caught` gave `32 records in 8 round(s), 201 ms` and `47 unmatched ticks in 8 round(s), 476 ms`, so the bound holds. Sampler checks passed in both.
- Guest duration of the original 5000-call loop on rel, measured with `clock_get_monotonic()` around it: 35.4 ms on an idle host, 25.4 ms in another boot. A round is some 25 to 35 sampling periods, so a short loop is not the explanation.
- Under host load (60 busy-loop Python processes on the 15-core Mac, `boot_check.py --no-build`, four runs): four runs, every verdict PASS, every check in one round. The load starved the timer thread as much as CI's did (calibration lost 12 to 34 of 31 to 53 PIT ticks), but no silent round came of it, so the CI failure itself was not reproduced locally; the fix is checked by the forced reruns above, and the cause rests on the CI log's evidence.

| Run | dbg PIT lost | dbg rounds, ms | rel PIT lost | rel rounds, ms |
|---|---|---|---|---|
| 0 | 34 of 53 | 1, 959 | 21 of 40 | 1, 34 |
| 1 | 23 of 42 | 1, 119 | 27 of 46 | 1, 43 |
| 2 | 29 of 48 | 1, 167 | 28 of 47 | 1, 43 |
| 3 | 25 of 44 | 1, 2282 | 12 of 31 | 1, 43 |

- `scripts/format_cpp.py --check` clean; clang-tidy reports nothing on the changed lines (the two statics moved in `check_unmatched_ticks()` were renamed `SINK` and `DUMP` for it).
- Not verified on the GitHub runner; the next CI run on the baseline is the check.

**Deliberately not done**
- No change to the sampler's design. `stop_locked()` clearing `ACTIVE` before late interrupts land is correct: a sample taken after the stop would be a write under the reader.
- `check_sampler_stress()` is left as it is: it checks the records it gets and asserts no minimum, so a starved run passes it.
- No attempt to make QEMU deliver timers from the vCPU threads (icount, `-accel tcg,thread=single`): both change the emulation for every check and slow the boot, to fix one check's assumption.

**How it was found**
- `grep -a sampl rel.log` in the run's artifacts: only `sampler read 0 records` failed; the kernel-only and stress checks after it passed, with low counts.
- `check_sampler()`'s comment put the window at "about 40 ms on dbg". A throwaway build printing `clock_get_monotonic()` around the loop gave 35 ms on rel, so the window was 35 periods on each of four CPUs, and getting none anywhere could not be chance in the guest.
- `session.cpp`, `timer_queue.cpp`, `tick_sched.cpp` and `tick_broadcast.cpp` were read for a way a due timer runs without recording: every path to `on_sample_due()` is an interrupt with a frame, and the one way a due timer is left unarmed, `ClockEventDevice::program_min_delta()` failing every retry, logs "no deadline could be programmed", which the failing log does not have. Nothing in the guest explained a silence on all four CPUs at once.
- The calibration warning in the same log, and the unmatched-tick counts across the two CI runs, pointed at the one host thread all four CPUs' timers share.
- Holding QEMU's main loop busy with monitor commands (`xp`) did not reproduce it: the BQL they hold also stops the shell's vCPU at its next interrupt, so the loop stretched to 780 ms and the late interrupts landed inside it (32 records). The CI case needs the main loop unscheduled while the vCPU runs freely, which a lock cannot imitate.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the shell's vCPU run 35 ms of guest time without a single timer interrupt under TCG, when every CPU has a sampler timer due after 1 ms?
- Why do the late interrupts record nothing once they arrive, and why is that the behaviour the stop needs?
- Why would a timed sleep inside the session guarantee a sample, while a guest-time deadline on the loop would not?
