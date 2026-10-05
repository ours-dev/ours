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

## The profiler takes the kernel's facts as arguments, and the arch seam and the PMU source are gone (2026-10-04)

**Files**
- `kernel/lib/prof/include/prof/arch.hpp` — deleted: the seven-function `prof::arch` seam
- `kernel/arch/x86/kernel/prof_pmu.cpp` — deleted: the seam's x86 implementation, the PMI sampling handler and the COM1 poller
- `kernel/arch/x86/kernel/faults.cpp`, `kernel/arch/x86/kernel/include/ours/arch/x86/faults.hpp`, `kernel/arch/x86/kernel/BUILD.gn` — the PMI vector warns and acknowledges instead of calling `x86_handle_pmi()`; the declaration, the build edges to `//kernel/lib/prof` and `//kernel/lib/unwind:headers` and an orphaned comment go
- `kernel/arch/x86/lib/arch/include/arch/x86/interrupt.hpp` — `IrqFrame::pc()`, `sp()`, `fp()` and `from_user()`
- `kernel/lib/prof/include/prof/sampler.hpp`, `kernel/lib/prof/src/sampler.cpp` — `SampleContext` gains `cpu` and `timestamp`; `Source`, `Config`, `active_source()`, `active_period()`, the `Stopping` state and the PMU branches are deleted; `start()` takes no argument
- `kernel/lib/prof/include/prof/sink.hpp`, `kernel/lib/prof/src/dump.cpp` — `LineWriter`; `export_dump()` and the hex-line helpers take one
- `kernel/lib/prof/include/prof/format.hpp` — `SampleRecord::tsc` now holds monotonic nanoseconds, said in a comment
- `kernel/kernel/sampler/sampler.cpp`, `kernel/include/ours/sampler/mod.hpp` — the timer handler builds the context itself; `start()` takes a `Duration`
- `kernel/commands/prof.cpp` — `prof.start [period-ns]`; `prof.dump` writes through the kernel logger
- `kernel/lib/prof/test/sampler_test.cpp`, `kernel/lib/prof/test/BUILD.gn`, `kernel/lib/prof/test/fake_arch.{hpp,cpp}` (deleted) — the tests feed contexts and collect lines directly; the fake goes
- `kernel/lib/prof/examples/trace_examples.cpp` — its host stubs move out of `prof::arch`
- `kernel/lib/prof/README.md`, `kernel/lib/kshell/README.md` — the new shape and command line
- `docs/repairs/261004/prof.md` (new) — kernel samples unwind no further than the interrupted PC

**Symptom**
None visible: a design problem. Of the seam's seven functions only the three PMU ones were about the machine. `current_cpu()`, `current_tid()`, `timestamp()`, `capture_sample_context()` and `debug_write()` were facts the kernel already owns, and their x86 implementation in `prof_pmu.cpp` called back up into `prof::Sampler::get()` from the PMI handler, so `kernel/arch` depended on the library above it. `debug_write()` polled the COM1 port itself, past the kernel logger and its lock, so a dump's bytes could interleave with a log line from another CPU.

**Cause**
The seam existed to let `kernel/lib/prof` build on its own, for the host tests. It did that by declaring functions and leaving the kernel to define them: a dependency through link-time symbols. That shape costs three things, and all three showed up here.

- Every program that links the library must link exactly one definition of every function. The host tests needed `fake_arch.cpp`, a file of global knobs (`set_current_cpu()`, `set_timestamp()`, `debug_output()`) that each test had to reset, so the test state was global even though `Sampler` takes everything else through `Resources`.
- The definitions land where the declarations point, not where the facts live. The seam was called "arch", so the CPU id, the thread id and the clock were implemented in `kernel/arch/x86`, which then needed `ours::task::Thread` and, for the PMI path, `prof::Sampler` itself. A layer below the library ended up calling into it, which is the dependency direction the seam was supposed to prevent.
- A declaration says nothing about when it is safe to call. `capture_sample_context()` only worked inside an interrupt whose preamble had recorded a frame; `debug_write()` only after sampling had stopped. Both rules lived in comments.

The other way to keep a library standalone is to take its inputs as arguments: the library states what it needs in a type, and whoever calls it gathers the values. `record_sample()` already took the registers in a `SampleContext`; the CPU and the time were the only inputs it still fetched. With them in the context, the library calls nothing outside itself, the host tests construct contexts by value, and the kernel side gathers each fact where it lives: the CPU from `Cpu::id()`, the thread from `Thread::Current`, the time from `SteadyClock`, and the registers from the frame. Reading pc, sp, fp and the privilege level out of an interrupt frame is the one thing that differs per architecture, and it belongs to the frame type, so it became four accessors on `arch::IrqFrame`.

The PMU source went with the seam. D2 retired it: QEMU TCG has no PMU, so the dev images never used it, and keeping it meant keeping PMU programming and the PMI handler inside the sampling session, which is the arch coupling this change removes. The PMU primitives in `arch/x86/pmu.hpp` stay for the facility that will own the PMU later.

**Change**
Before, the timer handler asked the seam:

```cpp
auto const context = prof::arch::capture_sample_context();   // in prof_pmu.cpp, x86
if (context.is_some()) {
    profiler.record_sample(*context);   // which then called arch::current_cpu(), arch::timestamp()
}
```

After, the handler builds the whole context from the kernel's own state:

```cpp
auto interrupted_context(arch::IrqFrame const &frame) -> prof::SampleContext {
    return prof::SampleContext{
        .pc = frame.pc(),
        .sp = frame.sp(),
        .bp = frame.fp(),
        .tid = static_cast<u32>(task::Thread::Current::tid()),
        .cpu = static_cast<u32>(Cpu::id()),
        .timestamp = static_cast<u64>(time::SteadyClock::now().time_since_epoch().count()),
        .from_uspace = frame.from_user(),
    };
}
```

The dump leaves through a caller-supplied writer, a function pointer and its context, the shape `unwind::MemoryReader` and `BufferLookup` already have:

```cpp
struct LineWriter final {
    void *context;
    auto (*write)(void *context, char const *bytes, usize len) -> void;
};
auto export_dump(LineWriter const &writer) const -> void;
```

`prof.dump` passes one that calls `log::get_global_logger().write()`, the path the `debug_write` syscall takes: raw bytes, no level or timestamp around them, so each `PROF1`/`SAMP` line stays parseable, and the logger's lock keeps a line whole on the wire.

Three smaller consequences:

- The record's timestamp is monotonic nanoseconds instead of the TSC. The TSC is an x86 fact, `SteadyClock::now()` is already read in the same handler by `Timer::forward()`, and the field only orders samples. The field keeps its wire name `tsc`, because `format_test` pins the layout and `prof-fold` does not read it.
- `Sampler` loses the `Stopping` state. It existed so that `stop()` could program the PMU off between Running and Stopped; now `stop()` is one Running -> Stopped exchange, and waiting for a writer that saw Running is the caller's job (`ours::sampler::stop()` does it with the `sync_exec()` cancel).
- With `m_source` and `m_period` gone, so is the race their comment admitted: a handler held off across a whole stop/start cycle could read the next session's period. The state word now publishes only the buffer reset.

Rejected: a `LineWriter` as a template parameter or a concept (a template would put the dump code in every caller's header and the host test would instantiate a different copy from the kernel's; one indirect call per line is nothing next to a UART); keeping `capture_sample_context()` as a kernel function outside `lib/prof` (the frame accessors make it a three-line function at its one call site).

**Reference**
Linux's `instruction_pointer()`, `user_stack_pointer()` and `user_mode()` over `struct pt_regs` (`arch/x86/include/asm/ptrace.h`), per-architecture accessors over the interrupt frame that arch-neutral code such as perf calls: the same shape as `IrqFrame::pc()` and `from_user()`. Zircon `sampler::ThreadSampler::SampleThread()` (`zircon/kernel/lib/thread_sampler/thread_sampler.cc`) reads pc and fp out of the iframe behind an `#ifdef` per architecture; the accessors are that switch, moved onto the frame type. The PMU as a facility of its own follows Zircon's former `zircon/kernel/lib/perfmon`; according to the research done on 2026-10-04 against Fuchsia checkout `51f05104c13`, upstream removed perfmon and mtrace on 2026-06-15 (commit `80c3a245853`), so the design's non-goal still has to be reworded.

**Verification**
Host: `//kernel/lib/prof` sampler tests 6/6, format tests 4/4, with no fake linked. `boot_check.py --out out/sampler`: dbg PASS (29 passed, 2 skipped), rel PASS (26 passed, 7 skipped, 2/2 required). clang-format clean on the changed lines; clang-tidy reports nothing in the touched files. On `dev.dbg_1` with `SMP=1`, through `kshell_run.py`: `prof.start 1000000` then `prof.stop` (9 samples), `prof.start 5000` answers InvArgs, `prof.dump` prints a `PROF1` header and 9 `SAMP` lines on the console, and `prof-fold` folds them against the dbg `vmours`: `tid 38;SerialLogger::write 7`, `tid 21;SerialLogger::write 2`.

Two things that run shows, neither caused by this change. Every sample has depth 1: the unwinder's first step finds no FDE for the interrupted PC, although `.eh_frame` has one covering it; the unwinder's inputs are field for field what the seam used to supply, so this predates the change, and it is `docs/repairs/261004/prof.md` item 1. And every sample's PC is `0x31f28`, the instruction after the `popfq` that ends `SerialLogger::write()`'s interrupts-off section: the dbg image spends its time printing `I'm Thread N`, the timer interrupt stays pending while the logger holds interrupts off, and it is taken the moment `popfq` turns them back on. That is the blind spot the design names, measured.

**Deliberately not done**
- `StackWindow` is still not testable (`docs/repairs/260910/prof.md` item 1); the frame-pointer walk that needs it arrives with the user target, step 6.
- The depth-1 unwinding is recorded, not fixed: it is a different module's question and a different commit.
- The `tsc` field is not renamed: a rename is a wire-format change for no reader.
- The other comments in `faults.cpp` and `ours/sampler/mod.hpp` that name other kernels are left for their own commit (`docs/repairs/261004/comment-rule.md` items 1 and 3); only the lines this change wrote follow the rule.

**How it was found**
None.

**The user's prediction**
None; the user asked for the steps to go ahead without stopping (2026-10-04).

**Three questions you should be able to answer**
- What upward dependency did the link-time `prof::arch` seam create, and why does taking the same facts in `SampleContext` remove it rather than move it?
- Why could the `Stopping` state and the `m_source`/`m_period` fields be deleted, and which race went away with them?
- Why does every timer sample in the dbg image land on the instruction after a `popfq`, and what does that say about profiling code that runs with interrupts off?

## A record buffer's depth is chosen when it is put to use, not when it is compiled (2026-10-04)

**Files**
- `kernel/lib/prof/include/prof/record_buffer.hpp` — `RecordBuffer<Record, Capacity>` becomes `RecordBuffer<Record>`: it borrows slots handed in by `attach(slots, capacity)` instead of holding a `std::array` of a compile-time size
- `kernel/lib/prof/include/prof/sink.hpp`, `buffer.hpp`, `sampler.hpp` — `RecordSink<Record>`, `SampleBuffer = RecordBuffer<SampleRecord>`, `Sampler::Sink` follow
- `kernel/commands/prof.cpp` — gives every possible CPU's buffer 128 slots from one allocation at boot
- `kernel/lib/prof/test/sampler_test.cpp` — the fixture attaches four slots per CPU; two new tests: a full buffer keeps its depth and counts the rest as dropped, and a buffer with no slots drops everything
- `kernel/lib/prof/examples/trace_examples.cpp` — attaches a 64-slot array before tracing
- `kernel/lib/prof/README.md` — the header table and the buffers paragraph

**Symptom**
None visible: a prerequisite. Step 4 of `.agents/tasks/prof-sampler-object` gives `sampler_create` a `buffer_size`, the bytes per CPU the session should keep, and every buffer was 128 records deep by its type.

**Cause**
The capacity was a template argument, so the depth was part of the buffer's type: `RecordBuffer<SampleRecord, 128>` and `RecordBuffer<SampleRecord, 4096>` are different classes, and `RecordSink`, `Sampler::Sink` and every lookup callback name one of them. A depth that a caller picks at run time cannot be a template argument, and making it one per supported size would mean one `Sampler` instantiation per size. It also put the slots inside the buffer object, about 19 KiB of them per CPU, so whoever allocated the per-CPU headers allocated the slots with them at a fixed size.

Separating the two answers both. The buffer keeps what the produce path touches (the cursor, a pointer, a count) and borrows the slots, which the owner allocates at whatever size it was asked for. The produce path is unchanged except that the bound it compares against is a member loaded from memory instead of an immediate, which is one more load on the same cache line as the cursor it has just incremented.

**Change**
Before:

```cpp
template <typename Record, usize Capacity>
class RecordBuffer final {
    static constexpr usize CAPACITY = Capacity;
    ...
    std::atomic<usize> m_write_index{0};
    std::array<Record, CAPACITY> m_slots{};
};
using SampleBuffer = RecordBuffer<SampleRecord, 128>;
```

After:

```cpp
template <typename Record>
class RecordBuffer final {
    auto attach(Record *slots, usize capacity) -> void;   // empties it too
    auto capacity() const -> usize;
    ...
    std::atomic<usize> m_write_index{0};
    Record *m_slots = nullptr;
    usize m_capacity = 0;
};
using SampleBuffer = RecordBuffer<SampleRecord>;
```

A buffer nobody attached has capacity 0 and drops every record, which is also how a CPU that is not possible behaves in the kernel: `prof_init()` attaches slots to the possible CPUs only. The kshell keeps 128 records per CPU, so nothing it reports changes; its slots come from one `new SampleRecord[possible * 128]` that lives as long as the buffers.

Rejected: a span member (`ustl::views::Span<Record>`) instead of a pointer and a count. It is the same two words, and the produce path would compare against `span.size()` either way; the header stays free of a ustl include that the host tests would then need.

**Reference**
None, this project's own design. Zircon sizes its per-CPU sampler buffers from `zx_sampler_config_t::buffer_size` at `ThreadSampler::Create()`, which is what this makes possible here.

**Verification**
Host: sampler tests 8/8 (two new), format tests 4/4. `trace_examples.cpp`, which no target builds, passes `-fsyntax-only` with the sampler test's flags. clang-format clean; clang-tidy reports nothing in the touched files. `boot_check.py --out out/sampler`: dbg PASS (29 passed, 2 skipped), rel PASS (26 passed, 7 skipped, 2/2 required). On `dev.dbg_1` with `SMP=1`: `prof.start 1000000`, `prof.stop` gives 9 samples.

**Deliberately not done**
- The slots of different CPUs are adjacent in one allocation, so the last record of one CPU and the first of the next can share a cache line. Only those two records do, and only when both CPUs write them at once; not worth an alignment rule until it is measured.
- The kshell's buffers still live for the whole boot. They move to a session allocated at create in the next change.

**How it was found**
None.

**The user's prediction**
None; the user asked for the steps to go ahead without stopping (2026-10-04).

**Three questions you should be able to answer**
- Why can a buffer depth chosen by a system call's argument not be a template parameter, and what would a template-per-size design cost?
- What does `try_produce()` do on a buffer with no slots attached, and why is that the right behaviour for a CPU that is not possible?
- What does the produce path pay for a run-time capacity, compared with a compile-time one?
