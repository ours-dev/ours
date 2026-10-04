# prof changes

## The profiler samples from per-CPU timers it arms itself, not from a hook on every scheduler tick (2026-10-04)

**Files**
- `kernel/kernel/sampler/sampler.cpp`, `kernel/kernel/sampler/BUILD.gn` (new) — `ours::sampler::start()`/`stop()`: one `time::Timer` per CPU, armed by `sync_exec()` on start, cancelled the same way on stop
- `kernel/include/ours/sampler/mod.hpp` (new) — the interface, and `MIN_PERIOD` (10 us)
- `kernel/kernel/time/tick_sched.cpp`, `kernel/kernel/time/BUILD.gn`, `kernel/include/ours/time/tick_sched.hpp` — the profiler call, its include and the build edge to `//kernel/lib/prof` are gone
- `kernel/lib/prof/include/prof/sampler.hpp`, `kernel/lib/prof/src/sampler.cpp` — `Source::TimerTick` becomes `Source::Timer` (period in nanoseconds); `on_scheduler_tick()` and its tick counter are deleted
- `kernel/commands/prof.cpp` — `prof.start [pmu|timer]` and `prof.stop` go through `ours::sampler`
- `kernel/lib/prof/test/sampler_test.cpp` — the tick test is deleted with the hook, the rest renamed
- `kernel/kernel/BUILD.gn` — links the sampler
- `kernel/lib/prof/README.md`, `kernel/lib/kshell/README.md`, `kernel/lib/prof/include/prof/arch.hpp` — say timer, not tick
- `.agents/tasks/prof-sampler-object/DESIGN.md`, `PLAN.md` — D6 decided, step 1 as built

**Symptom**
None visible: a design problem. Every tick of every CPU called `prof::Sampler::get().on_scheduler_tick()`, profiling or not, and `kernel/time` depended on `kernel/lib/prof` to do so.

**Cause**
The timer fallback was built as a consumer of the scheduler tick, like Linux's `perf_event_task_tick()` inside `scheduler_tick()`. That puts the cost of the profiler (a load and a branch, every tick) and the knowledge of it (an include and a build edge) into the timekeeping code, which has no reason to know a profiler exists. It also chains the sampling rate to HZ: the only way to ask for a rate was "every Nth tick".

The question a design should ask is who owns the decision "a sample is due". It is the profiler's, so the profiler should arm whatever tells it so, and only while it wants samples. The kernel already has the right object: a `time::Timer` armed with `activate()` queues on the calling CPU's `TimerQueue` and fires in that CPU's timer interrupt, which is exactly a per-CPU sampling clock. Zircon's `ThreadSampler` does this with `percpu::sampling_timer`, set by `ScheduleMarking()` from an `mp_sync_exec()` in `Start()`.

**Change**
Before, in `TickSched::on_tick()`:

```cpp
task::Scheduler::Current::tick(task::sched::to_sched_time(now));

// The sampling profiler's timer-fallback source samples the interrupted
// context here; a no-op unless 'prof.start tick' is running.
prof::Sampler::get().on_scheduler_tick();

TimerQueue::get().run_expired_timers();
```

After, `on_tick()` has no profiler line, and `ours::sampler::start()` arms the timers itself:

```cpp
auto const status = prof::Sampler::get().start(config);   // Running first
...
auto request = ArmRequest{&g_timers, period};
IpiService::sync_exec(Cpu::online_mask(), &arm_this_cpu, &request);
```

The handler (`on_sample_due()`) takes the interrupted context, records it, forwards the deadline by one period and returns `Restart`; once the sampler is no longer running it returns `Handled` instead, so it stops itself even before the cancel lands.

Three orderings carry the correctness, and each is written down where it happens:

- Running before arming. A timer that fires the instant it is armed must find the session running, or it records nothing and declines to re-arm.
- The timers are allocated by the first `start()`, not at boot, through `CpuLocal::allocate<time::Timer>()`, which placement-constructs every CPU's copy (a zero-filled Timer is not a constructed one). They are kept for later sessions.
- `stop()` stores Stopped, then cancels every timer with `sync_exec()`. The cancel is also the barrier the reader needs: a handler that loaded Running just before the store may still be writing, but it runs with interrupts off, so its CPU takes the cancel IPI only after it returns, and `sync_exec()` returns only after every CPU has taken it. When `stop()` returns, no CPU is writing a sample, so `prof.dump` reads whole records.

`start()` and `stop()` serialise on a `sync::Mutex`, Zircon's `ThreadSamplerLock`: a sleeping lock, because both hold it across `sync_exec()`, which needs interrupts on. Nothing on the sampling path takes it.

The new code is `ours::sampler` in `kernel/kernel/sampler`, not `ours::prof` in `kernel/kernel/prof` as the design first named it: inside namespace ours, an `ours::prof` would hide the library's own `prof`, and every `prof::Sampler` in kernel code would stop compiling or, worse, find the wrong one.

Rejected: keeping the tick hook and only making it cheaper (it is still the wrong owner); a timer handler that keeps re-arming until cancelled (correct, but one more pointless interrupt per CPU after stop).

**Reference**
Zircon `sampler::ThreadSampler::Start()` (an `mp_sync_exec()` that runs `ScheduleMarking()` on every CPU, arming `percpu::sampling_timer`) and `StopLocked()` (an `mp_sync_exec()` of `CancelMarking()`), in `zircon/kernel/lib/thread_sampler/thread_sampler.cc`. Linux `hrtimer_forward()` for the re-arm, as `Timer::forward()` already follows it.

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (20 passed), rel PASS (18 passed). Host: `//kernel/lib/prof` sampler tests 5/5, format tests 4/4. clang-tidy: no finding in the touched files. On `dev.dbg_1` with `SMP=1`: `prof.start timer 100000`, `prof.stop` (9 samples), `prof.start timer 200000`, `prof.stop` (9 samples), `prof.dump` (a `PROF1` header and the records); `prof.start timer 5000` answers InvArgs and `prof.start pmu` NotSupported (QEMU TCG). With 4 CPUs the first session sampled on every CPU (11 samples, 2 or 3 each).

The rate is one sample per ~10 ms whatever the period: the tick device stays Periodic, where `TickDevice::program_event()` does nothing and timers expire only from the tick. That is the kernel's timer resolution today (`docs/repairs/260903/kernel-time.md` item 6, `docs/repairs/260926/kernel-time.md` item 2), and the sampler gets finer for free once HpTick is entered.

**Deliberately not done**
- `prof::arch::capture_sample_context()` stays for now; it leaves in step 2 with the rest of the arch seam, when the caller fills the context itself.
- The PMU source is untouched; it is retired in step 2.
- No CPU-hotplug handling: a CPU that comes online during a session gets no timer. Zircon arms one from `sampler_percpu_init()`; this kernel has no hotplug.

**How it was found**
None.

**The user's prediction**
None; the user asked for the steps to go ahead without stopping (2026-10-04). The open prediction question from the design discussion (what goes wrong if the user stack is walked inside the timer interrupt) is still open.

**Three questions you should be able to answer**
- Why must `start()` mark the sampler Running before it arms the timers, and what would a timer armed first do?
- How does a `sync_exec()` of the cancel guarantee that no CPU is still writing a sample when `stop()` returns, and what property of the handler does that rely on?
- Why can the timer source not sample faster than once per tick today, and which change in the kernel would lift that without touching the sampler?
