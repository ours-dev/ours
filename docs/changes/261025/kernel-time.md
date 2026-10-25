# kernel-time changes

## preemptreset.bench counts what a pick costs the timer queue and device, and times the pieces (2026-10-25)

**Files**
- `benches/kernel/time/preempt_reset_bench.cpp`, `benches/kernel/time/BUILD.gn` — `preemptreset.bench`, a console command of the dbg image
- `benches/kernel/BUILD.gn` — linked into `//benches/kernel:commands`
- `kernel/kernel/time/timer_queue.cpp`, `kernel/include/ours/time/timer_queue.hpp` — the counters `timer.queue.preempt_resets` and `timer.queue.programs`, with `TimerQueue::num_preempt_resets()` and `num_programs()`
- `docs/repairs/261013/kernel-time.md` — item 1 gets the measurement
- `docs/repairs/261025/kernel-task.md` — item 1, what the measurement found

**Symptom**
None, the measurement the performance item `docs/repairs/261013/kernel-time.md` item 1 asked for before any candidate is chosen.

**Cause**
The item argued from reading the code that every pick of a thread takes the timer queue's lock and writes the timer device. That is a claim about counts, and counts can be measured on an emulator; the costs cannot.

**Change**
`preemptreset.bench` runs two threads on the highest online CPU that hand a token back and forth with two semaphores, 2,000 round trips, and reads the CPU's two new counters at either end. Then, on its own CPU with interrupts and preemption off, it times, in 100 batches of 100 calls each, a steady-clock read, a spin lock taken and released, `reset_preempt()` with the deadline the device holds, and `reset_preempt()` with a new earliest deadline each time, reporting the median and 99th percentile per call. The counts need no particular machine. Every timing rests on how fast the processor, the clock and the device answer, so each is `bench::Needs::Hardware` and is skipped under an emulator unless the command is given `all`.

A batch's 100 values are sorted with an insertion sort: `std::sort`'s out-of-line instantiation is not in the kernel, and the link failed on it.

**Reference**
None, this project's own measurement.

**Verification**
- Built in `.worktrees/wip-proc-self` at dd90ae72 plus only this change; `out/hotplug-off` and `out/hotplug-periodic` build too. `boot_check.py`: dbg and rel PASS, host tests 146 passed.
- On dbg under TCG: `per switch, on CPU 3: 1.99 deadline sets, 1.99 device writes (4000 switches)`, then the timings skipped. With `all`: a switch 17.6 us, a clock read 450 ns, a spin lock 190 ns, `reset_preempt()` unchanged 330 ns and new 4,280 ns, each flagged as not a hardware number.
- The 2 per switch, where 1 was expected, led to `docs/repairs/261025/kernel-task.md` item 1: a probe in `reschedule_common()`, not kept, counted 4,002 switches and 3,997 reschedules that kept the running thread.

**Deliberately not done**
Any change to what a pick does: the candidates in the repair item, and the stale preemption request, are the user's to choose among.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Which of the bench's numbers mean the same under QEMU as on hardware, and why not the others?
2. Why does `reset, same` cost so much less than `reset, new`?
3. Why did the hand-off set the deadline twice per switch, and what would the count be with the stale request consumed?
