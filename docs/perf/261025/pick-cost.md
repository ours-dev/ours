# pick-cost: performance report

**Date**: 2026-10-25
**Status**: Optimizing

## Question
What picking a thread costs its CPU's timer queue and timer device, and how much of a context switch that is. Repair item 1 of `docs/repairs/261013/kernel-time.md` says that every pick of a thread sets the CPU's preemption deadline (`TimerQueue::reset_preempt()`): it reads the clock, takes the timer queue's lock, and writes the timer device whenever the earliest deadline changed, which a new slice end always does. The item was argued from the code, with nothing measured. The user chose it as the first performance item to work on ("1可以", 2026-10-25), after its measurement method and candidates were laid out.

## Method
`preemptreset.bench` (`benches/kernel/time/preempt_reset_bench.cpp`), a console command of the dbg image.

- **Machine and build:** QEMU's TCG with `-cpu Nehalem` and 4 CPUs, on an Apple M5 Pro host; the dbg image built in `.worktrees/wip-proc-self` (`out/sampler-iso`), at the commit each stage names plus the bench's `csv` printing. Nehalem has no TSC-deadline timer, so the local APIC is written in its count mode, and the steady clock is the HPET.
- **Command:** `preemptreset.bench all csv` on the kshell, run three times per stage; the rows a run prints after `csv:` are kept, the three runs' together, in `pick-cost/<stage>.csv`.
- **Cases:**
  - `hand-off`: two threads on the highest online CPU pass a token back and forth with two semaphores, 200 round trips (400 switches) a sample, 10 samples a run.
  - `now()`, `lock`, `reset same`, `reset new`: on the bench's own CPU with interrupts and preemption off, in 100 batches of 100 calls, one sample per batch. They are, in order, a steady-clock read; a spin lock taken and released; `reset_preempt()` with the deadline the device holds already, so the lock and the compare without a device write; and `reset_preempt()` with a new earliest deadline each time, so the device is written.
- **Metrics:**
  - `deadline sets` and `device writes` per switch: the CPU's counters `timer.queue.preempt_resets` and `timer.queue.programs` across a sample. Counts, the same on any machine.
  - `time`: nanoseconds per switch or per call. Every one rests on how fast the processor, the clock and the device answer, which an emulator models not at all, so each is `bench::Needs::Hardware`, measured here only because of `all`, and **not a hardware number**: under TCG an MMIO access is a call into the emulator. They show where the time goes in this emulator, not on a machine.
- **Samples:** `hand-off` 30 a metric, the pieces 300 each, in every stage's file.

## Baseline
`pick-cost/baseline.csv`, at 5ff0d2a1, the parent of stage 1:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| hand-off | deadline sets | per switch | 30 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 | 0.000915 |
| hand-off | device writes | per switch | 30 | 2.00 | 2.00 | 2.00 | 2.00 | 2.00 | 0.000915 |
| hand-off | time | ns/switch | 30 | 12980 | 15651 | 21748 | 30035 | 17088 | 3794 |
| now() | time | ns/call | 300 | 400.0 | 470.0 | 500.0 | 650.0 | 470.4 | 33.5 |
| lock | time | ns/call | 300 | 170.0 | 220.0 | 260.0 | 420.0 | 216.4 | 32.2 |
| reset same | time | ns/call | 300 | 300.0 | 370.0 | 410.0 | 640.0 | 365.9 | 47.5 |
| reset new | time | ns/call | 300 | 1260 | 4230 | 5622 | 7960 | 4130 | 1334 |

Two numbers stand out. The hand-off sets the deadline and writes the device **twice per switch**, where reading the code says once: half the picks keep the thread that was running (stage 1). And in this emulator a device write dominates a pick: `reset new` costs about 3.9 us more than `reset same`, which is one clock read inside the device's programming plus the local APIC's two MMIO writes. Against that, the lock is 0.2 us and the compare is close to nothing. A clock read is 0.47 us, and a pick reads it twice, once to build the deadline and once to program the device.

## Stages

### Stage 1: a reschedule consumes the request for it
- **Commit:** 3449895f.
- **Decision:** the user's ("可以", 2026-10-25), after the extra half was traced, with a probe in `Scheduler::reschedule_common()` that was not kept, to reschedules in `Mode::Preempt` that picked the running thread again. A thread that woke another on its CPU kept the local preemption request it had set, past the switch that met it (`docs/changes/261025/kernel-task.md`). Prediction: none, homework being paused.
- **Its samples,** `pick-cost/stage-1.csv`:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| hand-off | deadline sets | per switch | 30 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 0 |
| hand-off | device writes | per switch | 30 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 0 |
| hand-off | time | ns/switch | 30 | 8005 | 10482 | 15735 | 19638 | 11253 | 3000 |
| now() | time | ns/call | 300 | 410.0 | 470.0 | 590.0 | 690.0 | 488.7 | 65.2 |
| lock | time | ns/call | 300 | 180.0 | 200.0 | 270.0 | 450.0 | 214.8 | 38.9 |
| reset same | time | ns/call | 300 | 300.0 | 330.0 | 420.0 | 610.0 | 357.0 | 52.5 |
| reset new | time | ns/call | 300 | 1260 | 4140 | 5162 | 9030 | 4044 | 1173 |

- **Gain over the baseline,** which is also the stage before:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| hand-off | deadline sets | per switch | 2.00 | 1.00 | 2.00x | -50.0% |
| hand-off | device writes | per switch | 2.00 | 1.00 | 2.00x | -50.0% |
| hand-off | time | ns/switch | 15651 | 10482 | 1.49x (within noise) | -33.0% |
| now() | time | ns/call | 470.0 | 470.0 | 1.00x (within noise) | +0.0% |
| lock | time | ns/call | 220.0 | 200.0 | 1.10x (within noise) | -9.1% |
| reset same | time | ns/call | 370.0 | 330.0 | 1.12x (within noise) | -10.8% |
| reset new | time | ns/call | 4230 | 4140 | 1.02x (within noise) | -2.1% |

- **Held:** the counts halve exactly, to 1.00 deadline set and device write per switch; the hand-off's median time falls by a third, though inside the emulator's spread. The pieces are unchanged, as they should be, since none of them took the path changed.

## Conclusion
None yet: stage 2 follows.
