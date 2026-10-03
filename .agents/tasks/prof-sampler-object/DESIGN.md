# prof-sampler-object: the sampling profiler as a kernel object userspace creates and drives

**Date**: 2026-10-02
**Status**: Proposed

## Problem

The profiler (`kernel/lib/prof`, driven by `kernel/commands/prof.cpp`) works, but has three problems.

- **The arch layer carries kernel facts.** `prof::arch` (`kernel/lib/prof/include/prof/arch.hpp`) has seven functions and only the three PMU ones are architectural. `current_cpu`, `current_tid`, `timestamp`, `capture_sample_context` and `debug_write` are facts the kernel already owns, pushed down into arch so that `lib/prof` could call itself standalone. The x86 file implementing them (`kernel/arch/x86/kernel/prof_pmu.cpp`) then calls back up into `prof::Sampler::get()` from the PMI handler, so arch depends on the library above it, and `debug_write` polls the COM1 port itself, bypassing the kernel console.
- **It costs where nobody asked for profiling.** `TickSched::on_tick()` (`kernel/kernel/time/tick_sched.cpp:60`) calls the profiler on every tick of every CPU, which makes `kernel/time` depend on `kernel/lib/prof`. `prof_init()` (`kernel/commands/prof.cpp:248`) builds a ~25 KiB FDE index and allocates the per-CPU buffers on every boot.
- **It is not an object.** It is a global singleton driven by three kshell commands. Userspace cannot reach it, no right gates it, and nothing ends a session when whoever started it goes away.

What forces it now: the user wants profiling to be a capability a user program holds, shaped after Zircon (2026-10-02).

## Goals and non-goals

Goals:

- A Sampler kernel object, created by a syscall from a System resource, that a user program starts, stops and reads.
- Zero cost while no session runs: no hook on the tick path, no buffers or unwind index allocated at boot.
- Two targets chosen in the config: user stacks (Zircon's ThreadSampler path) and kernel stacks (the existing in-interrupt `.eh_frame` unwind).
- The arch layer keeps only what is architectural: reading pc, sp, fp and the privilege level out of a register frame.

Non-goals:

- PMU-driven sampling. The PMU comes back later as a facility of its own, as Zircon's perfmon is (`zircon/kernel/lib/perfmon`, controlled through `zx_mtrace_control`), not as a sampler source. QEMU TCG has no PMU, so the dev images lose nothing; on KVM or hardware, cycle-based sampling is unavailable until then.
- Streaming reads while running, job policy (`ZX_POL_NEW_SAMPLER`), a boot-option gate like `kernel.enable-debugging-syscalls`, and symbolising user samples on the host.
- Tuning the sample path. Performance work goes through the user item by item.

## Design

### Objects

- **`SamplerDispatcher`** (`kernel/object`): the handle object, `ObjectType::Sampler = 36` (Zircon's `ZX_OBJ_TYPE_SAMPLER`). It stands for "this caller owns the session"; `on_zero_handles()` stops and destroys the session. Zircon: `zircon/kernel/object/sampler_dispatcher.cc`.
- **The session**: at most one system-wide. States `Unallocated -> Configured -> Running -> Stopping -> Configured`, and `Destroying` on the way back to `Unallocated`. It holds the per-CPU buffers and, when the kernel target is on, the FDE index. Zircon: `sampler::ThreadSampler` in `zircon/kernel/lib/thread_sampler/thread_sampler.cc`.
- **The per-CPU sampling timer**: a `time::Timer` per CPU, armed only while the session runs. `Timer::activate()` already arms on the calling CPU's `TimerQueue`, which is what Zircon's `percpu::sampling_timer` is.
- **The `SampleStack` thread signal**: a new bit in `task::ThreadSignals`. Zircon: `THREAD_SIGNAL_SAMPLE_STACK`, handled in `Thread::Current::ProcessPendingSignals()`.
- **The SAMPLING resource slot**: `RSRC_SYSTEM_SAMPLING_BASE = 15` (Zircon's value), minted by the kernel and handed to the first process, as VMEX is.
- **The `lib/prof` core**: stays host-testable. It keeps the wire format, `RecordBuffer`/`RecordSink`, the kernel-stack unwinder with its stack-window guard, and gains the user frame-pointer walk over an injected reader. It takes cpu, tid and timestamp as arguments instead of calling an arch seam.

### Syscalls

| Call | Arguments | Zircon |
|---|---|---|
| `sampler_create` | `rsrc`, `options` (must be 0), `config`, out `sampler` | `zx_sampler_create` |
| `sampler_start` | `sampler` | `zx_sampler_start` |
| `sampler_stop` | `sampler` | `zx_sampler_stop` |
| `sampler_read` | `sampler`, `data`, `data_size`, out `actual` | `zx_sampler_read` |

`SamplerConfig` holds `period` (nanoseconds, at least `SAMPLER_MIN_PERIOD` = 10 us, Zircon's value), `buffer_size` (bytes per CPU) and `targets` (`User`, `Kernel` or both). `targets` is ours; Zircon's third field is the iobuffer discipline. Each call gets its `ou::` entry.

### Events

1. `sampler_create`: check the resource is System at base SAMPLING, validate the config, allocate the per-CPU buffers and, if the kernel target is on, build the FDE index. The session becomes Configured and the caller gets a handle.
2. `sampler_start`: the session becomes Running, and `IpiService::sync_exec()` arms each online CPU's timer on that CPU.
3. A timer expires on CPU c, in interrupt context with interrupts off, on whichever thread it interrupted. It reads `irq::current_frame()`. If the frame is kernel code and the kernel target is on, it unwinds the kernel stack and writes the record now, as `Sampler::record_sample()` does today. If the frame is user code and the user target is on, it sets `SampleStack` on the current thread. Then it re-arms.
4. User target only: the marked thread reaches `process_pending_signals()` on its way to ring 3, sees `SampleStack`, enables interrupts, walks its user stack by frame pointers with `user_copy`, disables interrupts, checks the session is still running, and writes the record into this CPU's buffer.
5. `sampler_stop`: the session becomes Stopping, `sync_exec()` cancels each CPU's timer on that CPU, in-flight writers are waited for (D8), and the session is Configured again.
6. `sampler_read`: once stopped, every CPU's records are copied to the caller's buffer; a null buffer asks for the size needed.
7. The last handle closes: the session is stopped if running, the buffers are freed (D7), and it is Unallocated.

### Arch

After the change, what each architecture supplies is reading pc, sp, fp and "came from user mode" out of an `IrqFrame` (and, for the user target, the syscall frame): the `#ifdef` switch in Zircon's `ThreadSampler::SampleThread()`. The current CPU, the current thread, time, IPIs, timers and output are the kernel's own facilities, called directly.

### Placement

- `kernel/lib/prof`: the pure core; its host tests stay.
- The kernel-side session (timers, buffers, FDE index, state): `kernel/kernel/prof` (D6). `process_pending_signals()` in `kernel/kernel/task` and `SamplerDispatcher` in `kernel/object` both call it, so it has to sit below `kernel/object`.
- `kernel/object/sampler_dispatcher.{hpp,cpp}` and `kernel/lib/syscall/src/sampler.cpp`.
- Retired: `kernel/commands/prof.cpp` (the kshell `prof.*` commands and the boot-time `prof_init`), the `prof::arch` seam, the `tick_sched` hook and the COM1 `debug_write`.
- A user program under `examples/` drives a session and prints the existing `PROF1`/`SAMP` hex lines through `ou::debug_write`, so `prof-fold` keeps working for kernel samples. (2026-10-03: `ou` has been removed; that is `runtime::debug_write` now.)

### Concurrency

- Writers: the kernel target writes from the timer interrupt, with interrupts off. The user target writes from thread context, with interrupts off around the check-and-write.
- Readers: `sampler_read` copies to user memory, so it may fault and cannot keep interrupts off.
- Invariants: no writer touches a buffer once `sampler_stop` returns, and no buffer is freed while a read is copying from it.
- Zircon keeps the state and three counters (buffer references, write references, scheduled timers) in one 64-bit atomic and sleep-polls until the counters drain. That conflicts with the repo rule in `ours-kernel-design` (task.md, "Small concurrent state": never pack several fields into one atomic word). D7 and D8 are the two places this design departs; both are decided with the user when the step reaches them, with the user writing the invariant argument first.

## Alternatives considered

- **Keep the kshell commands and only move the hook off the tick path.** Fixes the cost but not the missing object, rights or lifetime.
- **Walk user stacks in the timer interrupt.** The reason it is rejected is recorded after the user answers the prediction question on it (D1).
- **One session per dispatcher, several at once.** The per-CPU timers, and later the PMU counters, are one system resource; Zircon allows one session and so does this.
- **A mapped buffer (VMO or IOBuffer) instead of copying out.** Zircon itself moved from returning an IOBuffer to `zx_sampler_read`. Copy-out needs no new object type; mapping can come back with streaming reads.
- **Frame pointers for kernel stacks too, dropping `.eh_frame`.** The user chose DWARF unwinding for the kernel in perf-instrument v1 (2026-08-30); nothing here changes that reason.
- **Keep the PMU as a sampler source.** Keeps cycle sampling on hardware, but keeps PMU programming and the PMI path tangled with the session, which is the arch coupling this design removes.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1 What is sampled | A: user stacks only (Zircon); B: kernel stacks only (today); C: both, chosen in the config | C | Not given | C, 2026-10-02 |
| D2 What triggers a sample | per-CPU `time::Timer` armed only while running; the scheduler-tick hook; the PMU | Timer; the PMU later as its own facility | Not given | Timer, 2026-10-02 |
| D3 Session model | one global session, the handle stands for it (Zircon); one session per dispatcher | One global session | Not given | One global session, 2026-10-02 |
| D4 Who may create one | System resource SAMPLING (15); plus a boot option and job policy | Resource only for now | Not given | Resource only, 2026-10-02 |
| D5 Shape of `lib/prof` | standalone through an arch seam (today); a pure core taking cpu, tid and time as arguments; merged into the kernel | Pure core | Not given | Pure core, 2026-10-02 |
| D6 Where the kernel-side session lives | `kernel/kernel/prof`; `kernel/lib/prof` itself (its host tests go); `kernel/object` | `kernel/kernel/prof` | | |
| D7 Who owns the buffers | the global session, with a reference count against a concurrent read and destroy (Zircon); the dispatcher, kept alive by the reference an in-flight syscall holds | The dispatcher | | |
| D8 How stop waits for writers | counters packed into the state word, sleep-polled (Zircon); clear the running state, then a `sync_exec()` on every CPU as a barrier, since every writer checks and writes with interrupts off | The barrier | | |

## Testing and verification

- Host: the `//kernel/lib/prof:tests` suites keep passing; the frame-pointer walk gets tests over a fake reader, and `StackWindow` becomes testable (repair item 1 in `docs/repairs/260910/prof.md`).
- Boot: `ours-verify`, both images, at every step.
- End to end: the user program in the dev image creates a session, spins, stops, reads, and reports a non-zero sample count per target; the kernel samples go through `prof-fold`.
- Refusals: create without the SAMPLING resource, a second create while a session exists, start twice, read while running, and closing the handle while running (a later create then succeeds).
- Not verifiable yet: sampling bias. A timer cannot interrupt code that runs with interrupts off, so such code is never sampled; the tick source had the same blind spot.

## Open questions

- Whether user programs are built with frame pointers, since `build/config` has `-momit-leaf-frame-pointer`, which drops a leaf function's caller from the walk.
- How the driving program receives the SAMPLING resource from the first process, as VMEX is handed on.
- The user-sample record: `SampleRecord` carries a tid but no process identity, and `prof-fold` symbolises against the kernel ELF only.
- The maximum `buffer_size` (Zircon allows 1 GiB per CPU).
