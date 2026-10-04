# prof-sampler-object: steps from the kshell profiler to a Sampler object

**Date**: 2026-10-02
**Status**: Proposed

## Request

The user (2026-10-02): the profiler leans too hard on the arch layer and samples where nobody asked for it; redesign it as an object-layer kernel object exposed to userspace, after Zircon. Then: "go with your recommendations" for D1 to D5 in `DESIGN.md`.

The scope ends at a user program that profiles user and kernel stacks through the object. The PMU facility, streaming reads and host symbolisation of user samples are out of scope.

## Decisions

D1 to D5 are decided; see `DESIGN.md`. The open ones, each put to the user before the step that needs it:

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D6 Where the kernel-side session lives (before step 1) | `kernel/kernel/prof`; `kernel/lib/prof`; `kernel/object` | `kernel/kernel/prof` | Not given | `kernel/kernel/sampler`, 2026-10-04 (see DESIGN) |
| D7 Who owns the buffers (before step 4) | the global session with a reference count (Zircon); the dispatcher | The dispatcher | | |
| D8 How stop waits for writers (before step 4) | packed counters, sleep-polled (Zircon); a `sync_exec()` barrier | The barrier | | |

## Steps

Each step was to be introduced to the user and wait for approval; on 2026-10-04 the user asked for the steps to be done without waiting, so each lands as its own verified commit and the decisions it needs take the recommendation, recorded as such.

1. **A per-CPU timer replaces the scheduler-tick hook.** `kernel/kernel/sampler` gets a sampling `time::Timer` per CPU, armed by `sync_exec()` when the sampler starts and cancelled the same way when it stops; the handler takes the interrupted context through `prof::arch::capture_sample_context()` (it leaves in step 2, with the rest of the seam) and calls `Sampler::record_sample()`. `Source::TimerTick` becomes `Source::Timer` with a period in nanoseconds; `Sampler::on_scheduler_tick()` and its tick counter go, and `tick_sched.cpp` loses its prof call and include. Why now: every later step's session sits on this trigger, and it takes the profiler off the tick path. Verified by the host tests, `ours-verify`, and `prof.start timer` / `prof.stop` on the `dev.dbg_1` image (the only one with the kernel shell since 2026-10-03) reporting samples. Commit: `refactor(prof): sample from a per-CPU timer, not the scheduler tick`.
2. **Kernel facts leave the arch seam.** The caller fills cpu, tid and timestamp into `SampleContext`; `export_dump()` takes a writer instead of `debug_write`; the PMU source and the seam half of `prof_pmu.cpp` are retired (the PMU primitives in `arch/x86/pmu.hpp` stay); `prof/arch.hpp` is deleted; arch supplies reading pc, sp, fp and the privilege level from an `IrqFrame`. Verified as in step 1, with the host tests no longer needing `fake_arch`. Commit: `refactor(prof): take kernel facts as arguments, not through an arch seam`.
3. **The SAMPLING resource slot.** `RSRC_SYSTEM_SAMPLING_BASE = 15`, minted by the kernel and handed to the first process beside VMEX. Verified by `ours-verify` and a check in the first process that the handle is there. Commit: `feat(object): mint the SAMPLING system resource for the first process`.
4. **The Sampler object, kernel target.** `ObjectType::Sampler`, `SamplerDispatcher`, `sampler_create`/`start`/`stop`/`read` in `syscalls.gni` callable as `ours::sampler_*()`; the session's states move into `kernel/kernel/sampler`, and buffers and the FDE index are allocated at create, not at boot. Verified by a user-space test of each call and each refusal. Commit: `feat(object): the Sampler object and its four syscalls`.
5. **A user program drives it; the kshell profiler retires.** A program under `examples/` creates a session, runs a workload, stops, reads and prints `PROF1`/`SAMP` lines; `kernel/commands/prof.cpp` and the boot-time `prof_init` are deleted. Verified by running `prof-fold` on its output. Commit: `feat(examples): profile from user space; retire the kshell prof commands`.
6. **The user target.** `ThreadSignals::SampleStack`; the timer marks a thread that it interrupted in user mode; `process_pending_signals()` walks the user stack by frame pointers with `user_copy` and writes the record. Verified by the program in step 5 reporting user samples from a user-space spin loop. Commit: `feat(prof): sample user stacks on the way back to user mode`.

## Files

- `kernel/kernel/sampler/`, `kernel/include/ours/sampler/` (new): the session, the per-CPU timers, buffers and FDE index.
- `kernel/kernel/time/tick_sched.cpp`: the prof call and include removed.
- `kernel/lib/prof/include/prof/sampler.hpp`, `src/sampler.cpp`: the timer source, the arguments that replace the seam.
- `kernel/lib/prof/include/prof/arch.hpp`: deleted.
- `kernel/lib/prof/test/`: `fake_arch.*` deleted; tests for the frame-pointer walk and `StackWindow`.
- `kernel/arch/x86/kernel/prof_pmu.cpp`: deleted; `faults.cpp` no longer calls `x86_handle_pmi()`.
- `kernel/commands/prof.cpp`: deleted in step 5.
- `system/public/ours/syscall/resource.hpp`: the SAMPLING slot.
- `system/public/ours/types.hpp`: `ObjectType::Sampler`.
- `system/public/ours/syscalls.gni`: the four calls (since `ou` was removed on 2026-10-03, the entry is all a call needs).
- `kernel/object/sampler_dispatcher.cpp`, `kernel/object/include/ours/object/sampler_dispatcher.hpp` (new).
- `kernel/lib/syscall/src/sampler.cpp` (new).
- `kernel/include/ours/task/types.hpp`, `kernel/kernel/task/`: the `SampleStack` signal and its handling.
- `examples/` (new program).

## Test plan

- Host: `//kernel/lib/prof:tests` pass at every step.
- Boot: `ours-verify` boots both images at every step.
- Steps 1 and 2: in the dev image, `prof.start timer` then `prof.stop` logs a non-zero `samples total`.
- Step 4: each call succeeds in order; create without the resource, a second create, start twice and read while running are refused with the documented status; closing the handle while running lets a later create succeed.
- Steps 5 and 6: the program reports non-zero kernel and user sample counts, and `prof-fold` turns the kernel samples into folded stacks naming kernel functions.
