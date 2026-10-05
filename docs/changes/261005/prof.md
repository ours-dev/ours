# prof changes

## A program profiles the kernel through a Sampler object and four system calls (2026-10-05)

**Files**
- `system/public/ours/sampler.hpp` (new), `public_headers.gni`, `vdso.hpp`, `kernel/lib/syscall/include/syscall/handlers.hpp` — `SamplerConfig`, `SamplerTargets`, `SAMPLER_MIN_PERIOD`, `SAMPLER_MAX_BUFFER_SIZE`
- `system/public/ours/types.hpp` — `ObjectType::Sampler = 36`
- `system/public/ours/syscalls.gni` — `sampler_create`, `sampler_start`, `sampler_stop`, `sampler_read`
- `kernel/object/sampler_dispatcher.cpp`, `kernel/object/include/ours/object/sampler_dispatcher.hpp` (new), `kernel/object/BUILD.gn` — `SamplerDispatcher`, owning a `sampler::Session`
- `kernel/lib/syscall/src/sampler.cpp` (new), `kernel/lib/syscall/BUILD.gn` — the four handlers
- `kernel/include/ours/sampler/mod.hpp`, `kernel/kernel/sampler/sampler.cpp` — `Session::dump_size()` and `Session::read()`, a reader count that `start()` refuses on, the limits taken from the ABI
- `kernel/lib/prof/include/prof/sampler.hpp`, `kernel/lib/prof/src/dump.cpp` — `Sampler::dump_size()`, `Sampler::copy_dump(ByteWriter)` beside the hex export, sharing `make_header()`
- `kernel/lib/prof/test/sampler_test.cpp` — the raw dump's layout, and a copy that stops at its first refusal
- `examples/shell/shell.cpp`, `shell_tests.hpp`, `shell_tests.cpp` — the shell takes the SAMPLING startup handle and runs `check_sampler()`
- `kernel/lib/prof/README.md` — the object as the session's second owner

**Symptom**
None: a new capability, step 4 of `.agents/tasks/prof-sampler-object`. Profiling was reachable only from the kernel shell, which no program can type into, and nothing checked who asked.

**Cause**
A capability a program holds needs three things this change adds: an object whose handle stands for it, calls on that object, and a gate on creating it. The gate is the SAMPLING resource minted in step 3; the object is `SamplerDispatcher`; and the session behind it (`sampler::Session`, the previous change) already did the work. What was left to design were the edges where user space meets the session.

- **Validation in two places.** `SamplerConfig` comes from user memory with a 64-bit `buffer_size`; the session takes a `usize`. On x86-64 the two are the same width, but on a 32-bit target a size past `usize` would wrap into one the session accepts, so the dispatcher checks the ABI limits before narrowing. The session checks again in its own terms, because the kernel shell calls it too.
- **The order of the checks.** `sampler_create` checks `options`, then the resource, then the configuration. A caller without the resource therefore learns nothing about which configurations would have passed: every refusal it can see is about the resource.
- **A read that holds no lock across its copy.** `sampler_read` copies to user memory, which can fault and, behind a pager, wait indefinitely. Holding the control lock through that would let one stuck reader block every create, start and stop in the system. So `Session::read()` takes the lock to check the state and count itself in, copies without it, and takes it again to count itself out; `start()` answers BadState while a read is counted in, because starting empties the buffers the copy is reading. Nothing else writes the buffers while the session is not running, and a release meanwhile leaves them alone, so the count is the only thing the copy needs. The reference the handler holds on the dispatcher keeps the memory alive if another thread closes the last handle mid-copy.
- **What a read returns.** The dump in its raw form: a `prof::DumpHeader`, then every CPU's retained `prof::SampleRecord`s in CPU order, the same structures the kshell prints as hex. `prof::Sampler::copy_dump()` produces it through a `ByteWriter` that receives pieces at offsets, so the library still knows nothing of user memory; the dispatcher's writer turns each piece into a `copy_array_to_user()` at that offset. A null buffer asks for the size; a short one answers BufferTooSmall with the size in `actual`; a fault ends the copy with `actual` at what was copied before it.

**Change**
The calls, as a program makes them:

```cpp
auto const config = ours::SamplerConfig{
    .period = 1'000'000,                 // ns
    .buffer_size = 8 * 152,              // bytes per CPU, whole records
    .targets = ours::SamplerTargets::Kernel,
    .reserved = 0,
};
ours::sampler_create(sampling_resource, 0, &config, &sampler);
ours::sampler_start(sampler);
// ... the workload ...
ours::sampler_stop(sampler);
ours::sampler_read(sampler, nullptr, 0, &needed);         // the size
ours::sampler_read(sampler, buffer, size, &actual);       // header + records
ours::handle_close(sampler);                              // ends the session
```

The handle carries Transfer, Duplicate and Inspect, and no call asks for a right: holding the handle is the capability. `on_zero_handles()` calls `Session::release()`, which stops a running session and frees the system's one slot, so the next create succeeds at once.

Rejected: rights on the calls (Write for start and stop, Read for read), which would let an owner hand a read-only handle to another process. Nothing needs that yet, and the object's rights follow its reference design until something does.

**Reference**
Zircon `sys_sampler_create`, `sys_sampler_start`, `sys_sampler_stop`, `sys_sampler_read` (`zircon/kernel/lib/syscalls/sampler.rs`): options must be 0, then `validate_ranged_resource(rsrc, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_SAMPLING_BASE, 1)`, then the configuration against `ZX_SAMPLER_MAX_BUFFER_SIZE` and `ZX_SAMPLER_MIN_PERIOD`; `SamplerDispatcher::DEFAULT_RIGHTS` is Transfer, Duplicate and Inspect, and the calls ask for no right (`zircon/kernel/object/sampler_dispatcher.rs`); `ZX_OBJ_TYPE_SAMPLER` is 36. Zircon's `ThreadSampler::ReadUser()` answers a null buffer with the size and a short one with INVALID_ARGS, and reads while running from per-CPU rings; this read answers BufferTooSmall and reads only once stopped (design decision D9).

**Verification**
Host: sampler tests 10/10 (two new), format tests 4/4. clang-format clean; clang-tidy reports nothing in the touched files. `boot_check.py --out out/sampler`, twice: dbg PASS (30 passed, 2 skipped), rel PASS (27 passed, 8 skipped, 2/2 required). On dbg, where the shell holds SAMPLING, `shell: sampler checks passed` with every line as expected: options 1 InvArgs; no resource BadHandle; a channel BadType for start, stop and read; VMEX OutOfRange; reserved 1, no target, an unknown target, a period of 9999 ns, a buffer short of a record and one past the maximum InvArgs; the user target NotSupported; a second create AlreadyExists, and still AlreadyExists with only a duplicate handle open; stop before start, start while running, read while running and stop twice BadState; the size query Ok, a buffer one byte short BufferTooSmall with the size, the read Ok with a header whose magic and record count match; 13 and 16 records in the two runs from a 5000-call loop of about 40 ms; and a create succeeding after the last handle of a running session was closed. On rel, where the root keeps the resource, the checks that need none pass and the sessions are skipped.

**Deliberately not done**
- The dump's structures are defined in `//kernel/lib/prof/include/prof/format.hpp`, not in `system/public`; the shell check spells out the two sizes and the magic it reads. Where a program gets them is step 5's question, with the program that prints `PROF1`/`SAMP` lines.
- No call reports a session's per-CPU statistics; the header's totals are what a reader gets.
- Rights on the calls: see Rejected.

**How it was found**
None.

**The user's prediction**
None; the user asked for the steps to go ahead without questions (2026-10-04).

**Three questions you should be able to answer**
- Why does `sampler_create` check the resource before the configuration, and what would a caller without the resource learn if the order were reversed?
- Why must `start()` refuse while a read is counted in, when `stop()` and `release()` need not?
- After another thread closes the last handle in the middle of a `sampler_read`, what keeps the buffers alive until the copy ends, and when are they freed?

## The sample dump's format is part of the system interface, defined with sampler_read (2026-10-05)

**Files**
- `system/public/ours/sampler.hpp` — `SamplerRecord`, `SamplerRecordFlags`, `SamplerDumpHeader` and their magics, `SAMPLER_MAX_STACK_DEPTH`, with layout asserts
- `kernel/lib/prof/include/prof/format.hpp` — the definitions leave; `prof::SampleRecord`, `prof::DumpHeader`, `prof::RecordFlags` and the constants become names for the public ones. The `PROF1`/`SAMP` text tags stay
- `kernel/lib/prof/src/sampler.cpp`, `kernel/lib/prof/cli/main.cpp`, `kernel/lib/prof/test/format_test.cpp` — `RecordFlags::Truncated`, `FromUser`, `None`; the record's `tsc` field is `timestamp`
- `system/public/ours/syscalls.gni`, `kernel/include/ours/sampler/mod.hpp` — `sampler_read` documents the public structures
- `examples/shell/shell_tests.cpp` — `check_sampler()` reads the header through `ours::SamplerDumpHeader` instead of spelling out offsets

**Symptom**
`sampler_read` (the previous change) hands a program bytes laid out by `prof::DumpHeader` and `prof::SampleRecord`, which live in `//kernel/lib/prof`. A program cannot include them without depending on a kernel library, which `//examples/BUILD.gn` exists to prevent, so the shell's check copied two sizes, a magic and an offset by hand.

**Cause**
A layout a system call writes into user memory is part of the interface between the kernel and every program, exactly like `SamplerConfig` going the other way, and the interface lives in `system/public`, which both sides compile against. It was in `kernel/lib/prof` because, until the previous change, the only readers were the kernel shell's hex export and the host `prof-fold` tool, both of which could reach the library. Keeping it there would leave every reader to copy the layout, and a copy drifts silently: the next field added to the record would break each one without a compile error.

Moving it also fixed the names to describe the data rather than its history. The record's `tsc` field has held monotonic nanoseconds since the arch seam went (2026-10-04), and the flags read `RecordFlags::RecordFromUspace`; as public names they are `timestamp` and `SamplerRecordFlags::FromUser`.

**Change**
Before, in `prof/format.hpp`:

```cpp
struct SampleRecord final { u32 magic; u8 depth; RecordFlags flags; ...; u64 tsc; u64 ips[16]; };
struct DumpHeader final { u32 magic; u16 version; ...; u64 load_base; };
```

After, in `<ours/sampler.hpp>`, with the same bytes:

```cpp
struct SamplerRecord { u32 magic; u8 depth; SamplerRecordFlags flags; ...; u64 timestamp; u64 ips[SAMPLER_MAX_STACK_DEPTH]; };
struct SamplerDumpHeader { u32 magic; u16 version; ...; u64 load_base; };
```

and `prof/format.hpp` keeps its own names as aliases (`using SampleRecord = ours::SamplerRecord;`), so the library, `prof-fold` and the tests change only where they named a renamed field or enumerator. Every offset is unchanged: `format_test` pins them, and the public header asserts them too.

Rejected: a new library for the format alone under `system/lib`. The structures are a system call's output, and the public headers are where every other such structure is (`PortPacket`, `InfoProcess`).

**Reference**
Zircon's sampler writes its samples as Fuchsia Trace Format profiler-backtrace records (`fxt::WriteProfilerBacktraceRecord()` in `zircon/kernel/lib/thread_sampler/thread_sampler.cc`), a format documented outside the kernel and serialised by `lib/fxt`, which kernel and user space share; a reader of `zx_sampler_read` never depends on the kernel's sampler library. Here the dump keeps this project's own layout, and the system headers are where both sides share it.

**Verification**
Host: sampler tests 10/10, format tests 4/4 (offsets as before). `prof-fold` builds and folds a kshell dump from `dev.dbg_1` (`SMP=1`): 9 samples, `SerialLogger::write` 7 and 2, as before the move. clang-format clean; clang-tidy reports nothing in the touched files. `boot_check.py --out out/sampler`: dbg PASS (30 passed, 2 skipped), rel PASS (27 passed, 8 skipped, 2/2 required); `shell: sampler checks passed` on both.

**Deliberately not done**
- The `PROF1`/`SAMP` text framing stays in `prof/format.hpp`: it is how a dump crosses a serial console to `prof-fold`, not what the kernel hands a program.
- The `DumpHeader` version stays 1: the bytes did not change.

**How it was found**
Writing `check_sampler()` in the previous change, which had to copy the layout.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What makes a structure part of the system interface rather than a library's private type, and where does this tree keep such structures?
- Why could the fields be renamed without changing the dump's version?
- What would go wrong, and when would anyone notice, if each reader of `sampler_read` kept its own copy of the record layout?

## A program profiles the kernel from user space, and the kernel shell's prof commands are gone (2026-10-05)

**Files**
- `examples/profile/profile.cpp`, `examples/profile/BUILD.gn` (new), `examples/BUILD.gn` — `bin/profile`: takes the SAMPLING resource, creates a session, runs 10 000 `vmo_create`/`handle_close` pairs, stops, reads, and prints a `PROF1` hex line, one `SAMPB` base64 line per record and a summary
- `kernel/products/image/dev.dbg_1/BUILD.gn` — the program goes into dbg_1's BootFS
- `examples/shell/BUILD.gn`, `examples/shell/shell_tests.cpp` — `user_programs::PROFILE`; `check_profile()` starts it with a duplicate of SAMPLING and expects exit 0
- `kernel/lib/prof/cli/main.cpp` — `prof-fold` reads `SAMPB <base64>` record lines beside `SAMP <hex>`
- `kernel/commands/prof.cpp` (deleted), `kernel/commands/BUILD.gn` — the kernel shell's `prof.start`, `prof.stop`, `prof.dump`, and the prof dependency
- `kernel/include/ours/sampler/mod.hpp`, `kernel/kernel/sampler/sampler.cpp` — `Session::export_dump()`, `stats()` and `num_cpus()`, which only those commands called
- `kernel/lib/prof/README.md`, `kernel/lib/kshell/README.md`, `kernel/products/image/dev/BUILD.gn` — the commands are no longer named
- `.agents/tasks/prof-sampler-object/PLAN.md`, `DESIGN.md` — step 5 as built; the open question narrowed to `dev`

**Symptom**
None: step 5 of `.agents/tasks/prof-sampler-object`. The object and its calls (2026-10-05) had only the shell's checks as a caller, and the kernel shell still carried its own way to profile.

**Cause**
Two things decided the shape.

How the program gets the resource. A capability reaches a process only by someone who holds it passing it on. On dbg_1, userboot hands SAMPLING to the shell (step 3), so the shell is the one that can give it to a program, and it does so the way it gives fault_probe its answer channel: as a startup handle in the processargs it writes. It hands a duplicate, and only to the program it starts for this check. Every command typed at the shell gets just its root region, as before, so `profile` typed by hand says it was started without the resource and exits 1. On dev nothing can profile yet: init receives the resource as a startup handle it never takes, and serving it through `/svc` the way init serves Debug is left open in the design.

How the dump crosses the console. `debug_write` takes at most `MAX_DEBUG_WRITE` (256) bytes per call, a bound that keeps a program from holding a CPU in the kernel with one huge write. A record is 152 bytes, so its hex line, `SAMP ` plus 304 digits plus a newline, is 311. Splitting it over two writes is not safe on dbg, where kernel threads print constantly: each write reaches the console whole, but another CPU's line can arrive between the two halves and cut the record in two. In base64 the same record is 204 characters and its line 211, one write. The kernel shell never had this problem because it wrote through the logger, which has no such bound; a program does not get that path.

With a program able to do everything the shell's three commands did, the commands went, as the design planned, and with them the session's three methods that existed for them.

**Change**
The program, in outline:

```cpp
auto const resource = ort::take_startup_handle(HandleType::SystemResource, u16(RSRC_SYSTEM_SAMPLING_BASE));
ours::sampler_create(resource, 0, &config, &sampler);   // 1 ms, 64 records per CPU, Kernel
ours::sampler_start(sampler);
workload();                                              // 10 000 vmo_create/handle_close
ours::sampler_stop(sampler);
ours::sampler_read(sampler, g_dump, sizeof(g_dump), &actual);
print_header(header);                                    // PROF1 <hex>
print_record(record);                                    // SAMPB <base64>, one debug_write each
```

`prof-fold` decodes either record line into the same `prof::SampleRecord`:

```cpp
} else if (text.starts_with("SAMP ") || text.starts_with("SAMPB ")) {
    ...base64_decode(text.substr(6), ...) or hex_decode(text.substr(5), ...)
```

Rejected: trimming a hex record to its used frames (24 bytes plus 8 per frame) so that it fits. That works up to 12 frames, and a deeper stack would lose its outermost frames, the ones a flame graph groups samples by. Raising `MAX_DEBUG_WRITE` was not considered: it is another module's bound, set for a reason of its own.

**Reference**
The program follows Fuchsia's `cpu_profiler` (`src/performance/cpu_profiler/kernel_sampler.cc`), which connects to `fuchsia.kernel.SamplingResource`, a protocol component_manager serves and `core.root_shard.cml` routes, takes the resource from it and calls `zx_sampler_create`; here the shell passes the resource directly, as dbg_1 has no root.

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required). On dbg, `profile: 28 samples, 0 dropped` and `shell: profile checks passed`; on rel, `shell: profile checks skipped (no SAMPLING resource)`. `prof-fold` over the dbg log's `PROF1`/`SAMPB` lines and the dbg `vmours`: 28 samples, among them the program's own thread (tid 2671) in `MemCache::allocate`, `MemCache::free`, `memset` and `operator new[]`, the kernel side of `vmo_create`; the rest are other CPUs in `SerialLogger::write` and the idle loop. The boot log no longer lists any `prof.` command. clang-format clean; clang-tidy reports nothing new in the touched files.

Every sample is still one frame deep (`docs/repairs/261004/prof.md` item 1).

**Deliberately not done**
- No route for the resource on dev (see Cause).
- No host test for `prof-fold`'s base64 decoder: it lives in the tool's anonymous namespace, and the boot run above exercises it on 28 real records.
- The library keeps `Sampler::export_dump()` and the hex line writer: the host tests use them, and a tracer will want a console form.

**How it was found**
None.

**The user's prediction**
None; the user asked for the steps to go ahead without questions (2026-10-04).

**Three questions you should be able to answer**
- Why does the shell give `bin/profile` a duplicate of SAMPLING rather than its own handle, and why does a program typed at the shell not get one?
- Why would splitting a 311-byte line over two `debug_write` calls corrupt the dump on dbg, when each call reaches the console whole?
- What did the kernel shell's `prof.dump` have that a program does not, which let it print hex lines of any length?

## The sampler records user stacks, walked by the interrupted thread on its way back to user space (2026-10-05)

**Files**
- `kernel/lib/prof/include/prof/sampler.hpp`, `kernel/lib/prof/src/sampler.cpp` — `walk_frame_pointers()`, over an injected `unwind::MemoryReader`, and `Sampler::record_user_stack()`
- `kernel/lib/prof/test/sampler_test.cpp` — five tests of the walk (the chain, missing memory, a chain that does not move up, a bad frame pointer, the room given) and two of the record
- `kernel/include/ours/task/types.hpp`, `kernel/include/ours/task/thread.hpp`, `kernel/kernel/task/thread.cpp`, `kernel/kernel/task/BUILD.gn` — `ThreadSignals::SampleStack`; `Thread::Current::request_stack_sample()`, which keeps where the tick landed on the thread and raises the bit; `process_pending_signals()` acts on it
- `kernel/include/ours/sampler/mod.hpp`, `kernel/kernel/sampler/sampler.cpp`, `kernel/kernel/sampler/BUILD.gn` — `Config::targets`; `Session::s_active`, the running session; the timer marks a user frame instead of skipping it; `Session::sample_user_stack()`; the unwind index only for the kernel target
- `kernel/object/sampler_dispatcher.cpp` — the user target is accepted
- `examples/profile/profile.cpp` — samples both targets over system calls and a user-space spin, counts each, and fails if either recorded nothing
- `examples/shell/shell_tests.cpp` — `check_sampler()` creates its sessions with both targets
- `.agents/tasks/prof-sampler-object/DESIGN.md` — how the two lost-tick cases came out

**Symptom**
None: step 6 of `.agents/tasks/prof-sampler-object`, the last. A session could record only kernel stacks; time a program spent in its own code was invisible.

**Cause**
The interrupt that takes a sample cannot read a user stack. Reading user memory can fault (a page not yet present, or swapped by a pager), and a fault in the timer interrupt, with interrupts off, has nowhere to go. That is design decision D1's prediction question, and the answer the design took: split the sample in two.

1. In the interrupt, when the tick landed in user space, the timer only marks the thread: it stores the interrupted pc and frame pointer on the thread and raises `ThreadSignals::SampleStack`, under the thread's PI lock, the lock Suspend and Kill are raised under from other CPUs.
2. On its way out of that same interrupt, the thread reaches `process_pending_signals()`, clears the bit, turns interrupts on and walks its own user stack by frame pointers through `user_copy`, where a fault is just a failed read that ends the walk.
3. Then, with interrupts off, it loads the running session and writes the record into this CPU's buffer.

Step 3 is what keeps the stop barrier (D8) valid for a writer that is not an interrupt. `stop` stores Stopped, clears `s_active`, and sends every CPU an IPI it waits for. A writer that loaded `s_active` before the clear holds interrupts off until its write is done, so its CPU takes the IPI only afterwards, and `stop` returns only after that; a writer that loads it after the clear finds null and records nothing. So when `stop` returns, no thread is writing into the session, and its memory can go.

The pc and frame pointer live on the thread rather than per CPU because the thread may move between steps 1 and 2: the interrupt's tail can preempt it before processing signals, and it can resume on another CPU. They need no lock, since only the thread touches them, in its own interrupt and on the way out of it.

**Change**
The mark, in the timer handler:

```cpp
if (!frame->from_user()) {
    if (!!(self->m_targets & SamplerTargets::Kernel)) {
        sampler.record_sample(interrupted_context(*frame));
    }
} else if (!!(self->m_targets & SamplerTargets::User)) {
    task::Thread::Current::request_stack_sample(frame->pc(), frame->fp());
}
```

The walk and the write, called from `process_pending_signals()` with interrupts on:

```cpp
auto const depth = prof::walk_frame_pointers(pc, fp, {nullptr, read_user}, ips, MAX_STACK_DEPTH);
arch::AutoInterruptDisabler const irqs_off;
auto *const session = s_active.load(std::memory_order_acquire);
if (session != nullptr) {
    session->m_sampler.record_user_stack(context, ips, depth);
}
```

The walk trusts nothing it reads: each frame pointer must be non-null, aligned and above the one before, and a zero return address ends it, so a corrupt or cyclic chain stops instead of looping.

Rejected: walking the stack in the interrupt with fault-capturing user copies (`copy_from_user_capture_faults()`). It would refuse a page that is merely not present yet, losing samples on any freshly touched stack, and it would put an unbounded walk of user memory into the interrupt.

**Reference**
Zircon `Thread::SignalSampleStack()`, the sampling timer's callback (`zircon/kernel/kernel/thread.cc`), sets `THREAD_SIGNAL_SAMPLE_STACK` on whatever thread is current, without a lock, and stores the session id on it; `Thread::Current::DoSampleStack()`, from `ProcessPendingSignals()`, skips a thread with no user half and calls `SamplerDispatcher::SampleThread()` with the user registers the return path saved, and `ThreadSampler::SampleThread()` walks by frame pointers with user copies (`zircon/kernel/lib/thread_sampler/thread_sampler.cc`). Three differences here: only a tick in ring 3 marks, so a mark cannot sit on a thread that never returns to user space; the pc and frame pointer are kept on the thread when it is marked, because `process_pending_signals()` is handed no registers; and the bit is raised under the PI lock, because the signals are a plain word every writer changes under it.

**Verification**
Host: sampler tests 17/17 (seven new), format tests 4/4. clang-format clean; clang-tidy reports nothing new in the touched files. `boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required). On dbg, `profile: 74 kernel samples, 17 user samples, 0 dropped` and `shell: profile checks passed`; `shell: sampler checks passed` with both targets. `prof-fold` folds the user records four frames deep; resolved against `bin/profile` at its load base `0x77c0200000`, the commonest is `spin_inner` (`0x1ffb`) → `main` (`0x1af0`) → `ort_start` (`0x25a3`) → `_start` (`0x210b`), 9 samples. `user_work`, `spin_inner`'s caller, is missing from every one of them: `spin_inner` is a leaf, built without a frame pointer (`-momit-leaf-frame-pointer`), so the walk starts from its caller's frame. That is the gap the design's open questions name.

**Deliberately not done**
- No count of ticks no target records: the dump header has no field left for it (DESIGN, Open questions).
- `prof-fold` still symbolises only kernel addresses; user frames print as addresses, and the record carries no process identity (DESIGN, Open questions).
- Leaf frames: building user code with leaf frame pointers is a build-wide choice, not this step's.

**How it was found**
None.

**The user's prediction**
None; the user asked for the steps to go ahead without questions (2026-10-04). D1's prediction question, what goes wrong if the user stack is walked in the interrupt, is answered above under Cause and Rejected.

**Three questions you should be able to answer**
- Why can the timer interrupt not walk a user stack, and what does it do instead?
- How does `stop`'s IPI barrier cover a user-stack writer, which is a thread rather than an interrupt, and what must the writer keep off for it to work?
- Why do the interrupted pc and frame pointer live on the thread rather than in a per-CPU slot?

## The kernel unwind reads only the interrupted thread's own stack, through a window that can be tested (2026-10-05)

**Files**
- `kernel/lib/prof/include/prof/stack_window.hpp` (new), `kernel/lib/prof/BUILD.gn` — `prof::StackWindow`: `on_stack(sp, base, top)` and `contains(address)`
- `kernel/lib/prof/include/prof/sampler.hpp`, `kernel/lib/prof/src/sampler.cpp` — `SampleContext::stack_base`/`stack_top`; `unwind_context()` reads through the window, and records a sample whose sp is off that stack as its pc alone, marked `Truncated`; the anonymous-namespace window with its 64 KiB reach is gone
- `kernel/kernel/sampler/sampler.cpp` — the timer handler names the running thread's kernel stack
- `kernel/lib/prof/test/sampler_test.cpp` — five tests: the window's bounds, the top of the address space, a window narrower than a word, and a kernel sample with no stack named
- `kernel/lib/prof/README.md` — how reads are bounded, and that user stacks are walked
- `docs/repairs/260910/prof.md` — its one item, the window cannot be tested, is fixed, so the file goes

**Symptom**
Latent until the unwinder works, which the next changes do. Every unwind step reads words the unwinder computed from frames it cannot trust (a saved rbp, a CFA), inside the timer interrupt, where a fault cannot be taken: a kernel-mode page fault there panics. The window that was meant to keep those reads on the interrupted stack let them reach 64 KiB above sp, while a kernel thread's stack is 16 KiB (`DEFAULT_STACK_SIZE`), a mapping of its own with nothing promised above it. And the window sat in an anonymous namespace, so nothing tested it (`docs/repairs/260910/prof.md` item 1).

**Cause**
The window guessed the stack's extent instead of asking. `lib/prof` cannot know where a kernel stack ends, so it took "64 KiB above sp" as a stand-in, and that is only safe if every stack is at least that big and nothing unmapped follows it, neither of which holds. The kernel does know: the interrupted code is running on the current thread's kernel stack, whose base and top `Thread::kernel_stack()` gives. The library's own rule is that the kernel hands it the facts it owns (2026-10-04, the arch seam's removal), and the stack's bounds are one more such fact, so they go into `SampleContext` beside the registers.

With real bounds the window becomes [sp, top): the frames of the interrupted function's callers are all above it, between it and the top. A sample whose sp is not on that stack -- code running on some other stack, or bounds the caller does not know -- has nothing that can be read safely, so it is recorded as its pc alone and marked as a prefix.

**Change**
Before, in `src/sampler.cpp`:

```cpp
constexpr u64 STACK_WINDOW = 0x1'0000;
auto window = StackWindow::above(context.sp);          // [sp, sp + 64 KiB)
```

After:

```cpp
auto window = StackWindow::on_stack(context.sp, context.stack_base, context.stack_top);
if (window.empty()) {                                  // sp is not on that stack
    record->flags |= RecordFlags::Truncated;
    return;
}
```

`contains()` keeps the two guards the old check had, now with tests: the comparison is `address > hi - 8`, never `address + 8 > hi`, which wraps near the top of the address space, and the address must be aligned.

Rejected: shrinking the reach to 16 KiB. It would match today's stack size and nothing else; a thread with a different stack, or an sp near the stack's top, would still read past it.

**Reference**
None, this project's own design. The unwinder's `MemoryReader` is the seam it has always had; what changed is who knows the bounds.

**Verification**
Host: prof sampler tests 22/22 (five new). clang-format clean; clang-tidy reports nothing in the touched files. `boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required). On dbg, none of the 253 kernel records in the `profile` run was marked `Truncated`: every sampled kernel sp, idle threads included, was on its thread's kernel stack.

**Deliberately not done**
- Interrupt stacks: the timer does not interrupt code running on one here, so the window does not cover them.

**How it was found**
The audit of what the task left (2026-10-05) put the 64 KiB reach beside the 16 KiB stack; the unwinder fixes that follow would have made it reachable.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the unwinder not be allowed to fault, and what did "64 KiB above sp" assume about kernel stacks?
- Why does the window start at sp rather than at the stack's base?
- Why is `address > hi - 8` safe where `address + 8 > hi` is not?

## A sampler_create that fails no longer deadlocks on the control lock (2026-10-05)

**Files**
- `kernel/kernel/sampler/sampler.cpp` — `Session::create()` builds and allocates the session before taking the control lock, and claims it inside a scope of its own
- `kernel/include/ours/sampler/mod.hpp` — a session starts `Released` (holding no claim) until `create()` claims it

**Symptom**
Latent on the dev images, which cannot make the allocation fail: a `sampler_create` whose configuration passes validation and whose buffers cannot be allocated left the creating thread parked for ever on the sampler's control lock, and every later sampler call, and every close of a sampler handle, behind it. With five or more CPUs a buffer size near the 1 GiB maximum is enough, because the slots of all CPUs are one allocation larger than the kernel's 4 GiB vmalloc window and fail at once.

**Cause**
`create()` took the control lock and then built the session in a `ustl::Box`:

```cpp
Guard<sync::Mutex> guard(s_lock);
...
auto session = ustl::Box<Session>::adopt(new (mm::GAF_KERNEL) Session(...));
if (auto const status = session->allocate(records_per_cpu); status != Status::Ok) {
    return status;
}
```

Locals are destroyed in the reverse order of their construction, so on that `return` the Box went first, while `guard` still held the lock. The Box's destructor runs `~Session()`, which calls `release()`, whose first statement takes the same lock. `sync::Mutex` is not recursive: the second acquire found the lock held, by this very thread, and parked waiting for a release only this thread could make, with no timeout and not interruptible.

The rule it broke is general: never destroy, under a lock, an object whose destructor takes that lock. The fix keeps the session out of the lock's scope entirely, and makes a session that never got the claim harmless to destroy.

**Change**
After:

```cpp
auto session = ustl::Box<Session>::adopt(new (mm::GAF_KERNEL) Session(...));  // starts Released
if (auto const status = session->allocate(records_per_cpu); status != Status::Ok) {
    return status;               // ~Session: release() finds Released and returns
}
{
    Guard<sync::Mutex> guard(s_lock);
    if (s_claimed) {
        return Status::AlreadyExists;   // the guard is destroyed first, then the session
    }
    s_claimed = true;
    session->m_state = State::Configured;
}
```

The claim is the only thing the lock has to cover. Allocating outside it also stops one create from holding every other sampler call while it builds the unwind index. Two creates racing both allocate, and the loser frees what it built.

Rejected: a flag that tells the destructor to skip `release()`. It would have fixed the deadlock and left the session's state saying Configured for a session nobody had claimed.

**Reference**
None, this project's own code. Upstream's `ThreadSampler::Create()` allocates inside `guard.CallUnlocked()`, with its control lock dropped, and re-checks the state afterwards (`zircon/kernel/lib/thread_sampler/thread_sampler.cc`), which is the shape this takes.

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required). The shell's sampler checks now exercise the new path: "a second create answers AlreadyExists" builds a whole session, loses the claim and destroys it after the lock is dropped, and does so twice in each boot without hanging. clang-tidy and the lock analysis report nothing.

Not exercised: an allocation that fails, which neither dev image can produce at the current limits.

**Deliberately not done**
- The maximum buffer size still lets one create ask for most of memory, where the allocator retries for ever instead of failing; that is the next entry.

**How it was found**
The audit of what the task left (2026-10-05), from reading the order of locals in `create()` against `release()`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- In what order are a function's locals destroyed on `return`, and why did that put `~Session()` inside the guard?
- Why does a non-recursive mutex make the second acquire park for ever rather than fail?
- Why is it safe for two creates to allocate at once, and what does the loser do?

## A sampling period has a maximum, so its deadline cannot overflow (2026-10-05)

**Files**
- `system/public/ours/sampler.hpp` — `SAMPLER_MAX_PERIOD`, one second; `SamplerConfig::period` names both bounds
- `kernel/object/sampler_dispatcher.cpp`, `kernel/include/ours/sampler/mod.hpp`, `kernel/kernel/sampler/sampler.cpp` — the dispatcher and the session refuse a longer period (`MAX_PERIOD`)
- `system/public/ours/syscalls.gni` — `sampler_create`'s doc gives the range
- `examples/shell/shell_tests.cpp` — `check_sampler()` tries a period one past the maximum

**Symptom**
Latent, reachable by any holder of the SAMPLING resource: `sampler_create` with a period near `INT64_MAX`, then `sampler_start`, locks every online CPU inside its timer interrupt for good.

**Cause**
Both checks bounded the period from below only. Starting a session arms each CPU's timer at `SteadyClock::now() + period`, and the handler re-arms it with `Timer::forward(period)`, all in signed 64-bit nanoseconds with no saturation. With a period near the maximum the first deadline wraps to a time far in the past; the next tick runs the handler, `forward()` overflows the other way and lands in the past again, and `run_expired_timers()` keeps finding the timer due and running it, with interrupts off, on every CPU `start()` armed. `stop()` and closing the handle can never run.

A period has a useful range, not just a useful minimum: at more than a second between samples a profile takes minutes to say anything. Bounding it there keeps the arithmetic about 63 bits away from overflowing.

**Change**
```cpp
inline constexpr i64 SAMPLER_MAX_PERIOD = 1'000'000'000;   // <ours/sampler.hpp>
if (config.period < SAMPLER_MIN_PERIOD || config.period > SAMPLER_MAX_PERIOD) { return Status::InvArgs; }
```

The session checks the same bound as a `Duration` (`MAX_PERIOD`), because the kernel can create a session without the dispatcher.

Rejected: saturating the deadline arithmetic in `Timer` instead. That belongs to the timer code and would still leave a session armed with a period no one can use.

**Reference**
Zircon checks only `ZX_SAMPLER_MIN_PERIOD` in `sys_sampler_create` (`zircon/kernel/lib/syscalls/sampler.rs`); the maximum is this project's own.

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required); on dbg `shell: sampler_create(period too long) = InvArgs` and `shell: sampler checks passed`. clang-tidy reports nothing in the touched files.

**Deliberately not done**
None.

**How it was found**
The audit of what the task left (2026-10-05), by tracing `Timer::forward()` with a period near the maximum.

**The user's prediction**
None.

**Three questions you should be able to answer**
- How does a huge period turn into a timer that expires for ever, and why does that lock the CPU rather than just one thread?
- Why check the bound in both the dispatcher and the session?
- What is the useful range of a sampling period, and what sets each end?

## One sampler_create can no longer ask for most of the kernel's memory (2026-10-05)

**Files**
- `system/public/ours/sampler.hpp` — `SAMPLER_MAX_BUFFER_SIZE` from 1 GiB to 4 MiB per CPU, with why
- `kernel/include/ours/sampler/mod.hpp` — `MAX_BUFFER_SIZE`'s comment says what happens when memory runs out, instead of promising NoMemory

**Symptom**
Latent, reachable by any holder of the SAMPLING resource: on the 512 MiB, 4-CPU dbg VM, a `sampler_create` with a buffer size of roughly 100 MiB per CPU or more, well under the old 1 GiB limit, takes every free frame and never returns. While it runs, other processes' page faults and the kernel's own allocations go without memory too.

**Cause**
A session allocates one block for every possible CPU's slots at create, so the request is the per-CPU size times the CPU count. The limit was 1 GiB per CPU, chosen as "large, and the allocation will answer NoMemory past what the machine has". It does not: `GAF_KERNEL` includes direct reclaim, and when the frames run out `alloc_frames_slowpath()` retries for ever, because reclaim and compaction are still stubs (`docs/repairs/260908/heap.md` item 3). So the limit is the only thing between a create and a machine with no free memory, and it has to be small enough on its own.

4 MiB per CPU is about 27,000 records: four and a half minutes of samples per CPU at the kernel's 100 Hz tick, longer than any session the dev images run, and 64 MiB at the 16 CPUs the kernel is built for.

**Change**
```cpp
inline constexpr u64 SAMPLER_MAX_BUFFER_SIZE = u64{4} << 20;   // was u64{1} << 30
```

Rejected: a limit on the total, computed from free memory at create. Free memory changes while the allocation runs, and the real fix for "the allocator never fails" belongs to the allocator.

**Reference**
Zircon allows `ZX_SAMPLER_MAX_BUFFER_SIZE`, 1 GiB per CPU, behind an allocator that fails when memory runs out (`zircon/system/public/zircon/syscalls/sampler.h`). This limit is lower because this kernel's allocator does not fail yet.

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required); on dbg `shell: sampler_create(buffer too large) = InvArgs` at the new limit plus one, and the profile run as before.

**Deliberately not done**
- The allocator retrying for ever is `docs/repairs/260908/heap.md` item 3; once it fails instead, the limit can go back up.

**How it was found**
The audit of what the task left (2026-10-05), from the allocation's size against the vmalloc window and the allocator's retry loop.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the request a create makes the per-CPU size times the CPU count, and when is it made?
- Why could the old limit not rely on the allocation answering NoMemory?
- What should change before the limit can be raised again?

## prof-fold writes each stack from its root, as flame graphs read it (2026-10-05)

**Files**
- `kernel/lib/prof/cli/main.cpp` — `fold()` writes a record's frames outermost first

**Symptom**
`flamegraph.pl` drew every profile upside down: each sampled function at the bottom of its column, its callers stacked above it, and no shared roots to merge on.

**Cause**
The folded format `flamegraph.pl` reads is one line per stack, frames separated by `;` from the root of the stack to its leaf. A record lists its frames the other way, the interrupted pc first and each caller after it, and `fold()` wrote them in record order. The frames were right; only their order on the line was reversed, so the graph's merging of common callers, which is what makes a flame graph readable, never happened.

**Change**
```cpp
for (usize i = record.depth; i-- > 0;) {   // was: for (usize i = 0; i < record.depth; ++i)
```

The `[user]` mark stays on the record's first frame, now the end of the line.

**Reference**
The folded-stack format of Brendan Gregg's `stackcollapse-*.pl` scripts, which `flamegraph.pl` reads: one line per stack, its frames separated by `;` from the root to the leaf, then a space and the sample count.

**Verification**
`prof-fold` over a dev.dbg_1 dump now prints `tid 35;ours::task::Thread::trampoline;…;logz4::vlog<…>;SerialLogger::write 25` and, for the user spin, `_start` → `ort_start` → `main` → `spin_inner [user]`. `boot_check.py --out out/sampler`: dbg PASS, rel PASS (no kernel change). clang-tidy reports nothing in the file.

**Deliberately not done**
None.

**How it was found**
The audit of what the task left (2026-10-05).

**The user's prediction**
None.

**Three questions you should be able to answer**
- In which order does the folded format list a stack's frames, and in which order does a sample record?
- Why does the order on the line decide whether a flame graph merges common callers?
- Which frame does the `[user]` mark belong to, and where is it on the line now?

## A user-stack mark is recorded only into the session that made it (2026-10-05)

**Files**
- `kernel/include/ours/sampler/mod.hpp`, `kernel/kernel/sampler/sampler.cpp` — `Session::m_generation`, numbered from `s_next_generation`; the timer leaves it on the thread it marks, and `sample_user_stack()` writes only if the running session has the same one
- `kernel/include/ours/task/thread.hpp`, `kernel/kernel/task/thread.cpp` — `m_sample_session`, the token `request_stack_sample()` keeps and `process_pending_signals()` hands back

**Symptom**
Latent and rare: a sample taken for one session could be written into the next. It needs a thread marked by session A's timer that acts on the mark only after A has stopped and session B has started.

**Cause**
A mark is a bit, a pc and a frame pointer on the thread, with nothing saying which session asked. The thread acts on it on its way out of the timer interrupt, but not always at once: the interrupt's tail can preempt it first, and a suspend can come before the sample too. Meanwhile the profiler can stop, close, create and start again. `sample_user_stack()` wrote into whatever session was running at write time, so B got a sample its own timers never took, and a session's records stopped being exactly the samples taken while it ran. The comment on the late case said the record would be refused because "the session may have stopped", which is only half the cases.

**Change**
Each session takes a number when it is built, and the mark carries it:

```cpp
task::Thread::Current::request_stack_sample(frame->pc(), frame->fp(), self->m_generation);
...
auto *const session = s_active.load(std::memory_order_acquire);
if (session == nullptr || session->m_generation != generation) {
    return;
}
```

A number rather than the session's address: a later session can be allocated where an earlier one was freed, and the address would then match.

**Reference**
Zircon's `Thread::SignalSampleStack()` stores the session id on the thread (`sampling_session_`), and `ThreadSampler::SampleThread()` refuses a sample whose session id is not the current one (`zircon/kernel/kernel/thread.cc`, `zircon/kernel/lib/thread_sampler/thread_sampler.cc`).

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required); on dbg `profile: 117 kernel samples, 22 user samples` and both sampler checks pass. clang-tidy reports nothing in the touched code.

Not exercised: the late case itself, which needs a stop, close, create and start to fit between a mark and its walk.

**Deliberately not done**
None.

**How it was found**
The audit of what the task left (2026-10-05).

**The user's prediction**
None.

**Three questions you should be able to answer**
- How can a thread act on its sample mark after the session that made it has ended?
- Why compare a generation number rather than the session's address?
- What property of a session's records does the check keep?

## The sampler's documents say what the code does now (2026-10-05)

**Files**
- `system/public/ours/sampler.hpp` — the user target, `FromUser`, `Truncated`, `cpu` and `timestamp` describe user records as they are built
- `system/public/ours/syscalls.gni` — `sampler_create` no longer says the user target is NotSupported; `sampler_read`'s BufferTooSmall case says |actual| may be 0
- `kernel/object/include/ours/object/sampler_dispatcher.hpp`, `kernel/object/sampler_dispatcher.cpp` — the same NotSupported line goes
- `kernel/include/ours/sampler/mod.hpp`, `kernel/kernel/sampler/sampler.cpp` — at most one session holds the claim, not exists; the stop barrier's comment describes the user-stack writer as built
- `kernel/lib/prof/include/prof/sampler.hpp`, `kernel/lib/prof/include/prof/trace.hpp`, `kernel/lib/prof/README.md`, `kernel/lib/prof/cli/main.cpp` — both dump writers, the session's `stop()`, the tests' real paths, no PMU sampler to contend with, prof-fold's help text
- `examples/profile/profile.cpp`, `examples/profile/BUILD.gn`, `examples/shell/shell_tests.cpp` — both workloads and both targets; the header line is 71 characters
- `kernel/products/image/dev/BUILD.gn`, `kernel/lib/kshell/README.md` — only dev.dbg_1 starts the kernel shell
- `.agents/tasks/prof-sampler-object/DESIGN.md`, `PLAN.md`, `RESULT.md` — status, real session states, D1's rejection reason, the dbg image, the commits after step 6
- `docs/changes/261005/prof.md` — prof-fold's Reference describes the folded format instead of quoting a sentence its source does not contain

**Symptom**
An audit of the task's files after step 6 found statements the code had stopped making true: the system interface still said the user target answered NotSupported, `SamplerRecord::FromUser` said only `ips[0]` meant anything, the dev image's build file said it started the kernel shell (its command line says `kernel.shell=false`), and the design still listed a `Stopping` state and a "Destroying" path that were never built.

**Cause**
Each step changed code and left the comments of the step before. Step 6 made the user target real but did not revisit the five places that described it as missing, and the retirement of the kernel shell's `prof.*` commands left their names in the library's README and the host tool's help. A comment that contradicts the code is worse than none: the interface header is what a program's author reads, and it told them a call would fail that succeeds.

**Change**
Comments and documents only; no code changed. Before, in `<ours/sampler.hpp>`:

```cpp
/// The tick interrupted user code: only ips[0], the user pc, means
/// anything.
FromUser = 1,
```

After:

```cpp
/// The tick interrupted user code: ips are the user stack, the
/// interrupted pc and then each frame's return address, walked by frame
/// pointers. A function built without one leaves its caller out.
FromUser = 1,
```

The rest follow the same pattern; the Files list says what each says now.

**Reference**
None, this project's own documents.

**Verification**
`lint_docs.py --changed` clean on every document; the formatter reports every changed C/C++ and GN file already formatted. Host: sampler tests 22/22, format 4/4, unwind 9/9, cfi 11/11. `boot_check.py --out out/sampler`, twice: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required) both times.

**Deliberately not done**
The design stays in `.agents/tasks/` rather than moving to `docs/design/`: D7 to D9 were taken without the user's own argument, so it is not yet accepted.

**How it was found**
A read of every file the task touched against the code as committed, after the user asked what was still unfinished.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is a stale comment in `system/public` worse than one inside the kernel?
- Which flag says a kernel record's stack is only a prefix, and why does a user record never carry it?
- Why does `SamplerRecord::cpu` for a user record name the CPU that walked the stack rather than the one the tick landed on?

## profile alternates its two workloads, so a slow CPU still records both (2026-10-05)

**Files**
- `examples/profile/profile.cpp` — the workload runs as 40 rounds of 250 system-call pairs and then 500,000 additions, instead of all the system calls and then all the additions; `user_work()` may not end in a tail call
- `docs/repairs/261005/kernel-top.md` — new: on dbg, work placed on CPU 0 sometimes runs fifty to a hundred times slower
- `.agents/tasks/prof-sampler-object/RESULT.md` — the verification counts and the end-to-end numbers as of this commit; what is not done

**Symptom**
The dbg boot's `profile` check passed with `profile: 255 kernel samples, 1 user samples, 6376 dropped`, against `74 kernel samples, 17 user samples, 0 dropped` when step 6 landed. One user sample is one short of failing: profile exits 2 when a target recorded nothing, and the check, and with it `boot_check.py`, fails. Of the dozen dbg runs on 2026-10-05 whose profile line was kept, three were like that (1, 3 and 1 user samples).

**Cause**
The check measured a fixed amount of work with a clock it does not control. Each CPU keeps `RECORDS_PER_CPU` = 64 records, about 0.64 s of 100 Hz ticks, and once a CPU's buffer is full every later sample on it is dropped. profile made its 10,000 system calls first and did its user arithmetic after. That is fine while the whole workload takes about 0.25 s, but in the runs above the session lasted 14 to 24 s, by their drop counts. In the two whose records were decoded, the workload had landed on CPU 0, which then executed every instruction fifty to a hundred times slower (`docs/repairs/261005/kernel-top.md` item 1). The system calls alone outlasted the buffer, so by the time the arithmetic began there was nowhere to put a user sample. The one user sample that survived was a tick that landed in the user code between two system calls.

The order of the two phases turned a slowdown into a failure. Any check that does fixed work but reads a time-based measurement has this shape: it holds only while the work fits the measurement's window.

**Change**
The two workloads alternate in short rounds, with the same total work:

```cpp
// Before
kernel_work();          // 10,000 vmo_create/handle_close pairs
auto _ = user_work();   // 20 calls of spin_inner(1'000'000)

// After
for (auto round = 0; round < ROUNDS; ++round) {   // ROUNDS = 40
    kernel_work();          // 250 pairs
    auto _ = user_work();   // spin_inner(500'000)
}
```

Now the records a CPU keeps hold both kinds whatever the speed, as long as one round's system calls fit a buffer: at a hundred times slower, 250 pairs take about 18 ticks of the 64.

`user_work()` became a single call, `return spin_inner(...)`, which clang compiles to a `jmp`: the tail call removes `user_work`'s frame, and since `spin_inner` is a leaf built without a frame pointer, the walk would then start from `main`'s frame and miss `main` itself. `[[clang::disable_tail_calls]]` keeps the `call`, and the user stacks keep their shape (`spin_inner` → `main` → `ort_start` → `_start`).

Rejected:

- A time-bounded workload, which is the real fix for the shape: a program cannot read a clock yet (the syscall documents answer NotSupported for deadlines "until a program can read a clock").
- Larger buffers: 20 s of samples is 2,000 records a CPU, and profile prints every record as a `SAMPB` line of about 210 characters through the same contended console.
- User work first: then a slow run drops the workload's kernel samples instead. The check would still pass on the other CPUs' kernel samples, but the profile would no longer show the system calls it was built to show.

**Reference**
None, this project's own test program.

**Verification**
`boot_check.py --out out/sampler`, twice: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required) both times. The first run was a slow one: `profile: 236 kernel samples, 20 user samples, 4043 dropped`, and the workload's CPU, CPU 0, kept 44 kernel and 20 user records where the run before the change kept 63 and 1. The commonest user stack, 16 of the 20, is `spin_inner` → `main` → `ort_start` → `_start`. The second was fast: `profile: 56 kernel samples, 8 user samples, 0 dropped`, the workload on CPU 1 with 8 records of each kind in 0.15 s. `llvm-objdump` shows `user_work()` as `push %rbp; mov %rsp,%rbp; call spin_inner; pop %rbp; ret`.

**Deliberately not done**
The slowdown itself: it is not the sampler's, its cause is not known, and a performance problem goes to the repairs list, not into a fix made in passing.

**How it was found**
Running `boot_check.py` for the documents commit printed `255 kernel samples, 1 user samples, 6376 dropped`. Decoding the `SAMPB` records by CPU and thread showed each CPU's 64 records full: the debug threads on two CPUs, an idle thread on one, and profile alone on CPU 0. The kernel log's timestamps placed the system calls at about 8 s and the arithmetic at about 8.5 s, and the next boot ran the same workload on CPU 1 in 0.24 s, with six other checks slow instead.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did the order of the two workloads decide whether a slow run failed, when the total work was the same?
- Why does a tail call from `user_work` to `spin_inner` cost the user stack `main` as well as `user_work`?
- What would let profile bound its session by time instead of by work, and why can it not do that today?

## prof-fold prints user frames at their own addresses (2026-10-05)

**Files**
- `kernel/lib/prof/cli/main.cpp` — `fold()` no longer subtracts the kernel's load base from a `FromUser` record's ips, nor looks them up in the kernel's symbols
- `docs/repairs/261005/prof.md` — the item is removed

**Symptom**
Every user frame came out shifted: `profile`'s `spin_inner` at `0x202000` printed as `0x77c0202000`. The step 6 entry above took one such number, `0x77c0200000`, for `bin/profile`'s load base; the program is in fact loaded at `0x200000`.

**Cause**
`fold()` computed every frame as `ips[i] - load_base - extra_base`. `load_base` is the kernel image's runtime base, which the ABI defines for kernel pcs only. Subtracting it from a user address wraps to `u + 2^64 - load_base`, which with the default base `0xffffff8840000000` is `u + 0x77c0000000`: a number in no binary, which nobody can symbolise by hand.

**Change**
A `FromUser` record's frames are printed as they are, and only kernel records are rebased and looked up:

```cpp
if (from_user) {
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(record.ips[i]));
    key += buf;
    ...
    continue;
}
u64 const ip = record.ips[i] - dump.header.load_base - extra_base;
```

**Reference**
None, this project's own tool.

**Verification**
On a dump captured from dbg on 2026-10-05, the commonest user stack now folds as `tid 2671;0x2020cb;0x202563;0x201ac5;0x202000 [user] 16`: `_start`, `ort_start`, `main` and `spin_inner` at their addresses in `bin/profile` (`llvm-nm`). Kernel stacks fold as before. The boot images do not contain the tool; the build and both boots pass with it.

**Deliberately not done**
Symbolising user frames, which needs the record to say which process and binary it came from (an open question of the design).

**How it was found**
The audit of 2026-10-05 (two auditors).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does subtracting the kernel's load base from a user address give `u + 0x77c0000000`?
- Which field of the dump says which base applies to which records?
- What would prof-fold need to name user frames, and why does the dump not have it yet?

## A kernel stack counts as whole only if it ends where the entry code began it (2026-10-05)

**Files**
- `kernel/arch/x86/kernel/syscall.S`, `kernel/arch/x86/kernel/exception.S` — global labels on the return addresses of the entry stubs' calls into C++
- `kernel/lib/arch/include/arch/stack_root.hpp`, `kernel/arch/x86/lib/arch/include/arch/x86/stack_root.hpp`, `kernel/lib/arch/BUILD.gn` — new: `arch::stack_roots()`, each a return address and its exact depth below the top of the thread's kernel stack
- `kernel/lib/prof/include/prof/sampler.hpp`, `kernel/lib/prof/src/sampler.cpp` — `prof::StackRoot`; a walk ending at a frame with no unwind information is whole only at a root
- `kernel/lib/thread_sampler/include/thread_sampler/thread_sampler.hpp`, `kernel/lib/thread_sampler/src/thread_sampler.cpp` — hand the arch's roots to the sampler
- `system/public/ours/sampler.hpp` — `Truncated` says so
- `kernel/lib/prof/test/sampler_test.cpp` — new `AFrameWithNoUnwindInfoEndsTheStackOnlyAtARoot`
- `docs/repairs/261005/prof.md` — the item is removed

**Symptom**
A kernel record could be cut short with no `Truncated` flag. Captures on 2026-10-05 had records of one frame at `x86_user_copy`, from a thread whose other records ran down to `x86_syscall`: a user copy's callers and the system call under them were gone, and the record claimed to be the whole stack. The same happened for an exception taken in kernel mode, such as a page fault in a user copy, and for every record if the unwind index could not be built.

**Cause**
The unwinder ends a walk at a frame it has no FDE for, and `unwind_context()` took every such end as the bottom of the stack, on the belief that only entry stubs lack FDEs. But all kernel assembly emits its CFI into `.debug_frame` only (`kernel/debug_frame.S`), which the sampler does not read, so every assembly routine ends a walk, wherever it is. `x86_user_copy` is a leaf called from C++ in the middle of a stack, and `interrupt_common` sits mid-stack when the exception came from kernel mode.

**Change**
The end of a stack is now declared by the code that begins it, not assumed. An entry from user mode lands on the top of the thread's kernel stack (TSS.sp0, and the per-CPU kernel stack pointer for SYSCALL, are both set to it at every switch). The stub builds its frame there and calls into C++, so the call's return address sits exactly the frame's size below the top: 144 bytes for a `SyscallFrame`, 176 for an `IrqFrame`. The stubs label those return addresses, and the arch lists them:

```cpp
{x86_syscall_dispatch_return, sizeof(SyscallFrame)},
{x86_syscall_signals_return, sizeof(SyscallFrame)},
{x86_exception_return, sizeof(IrqFrame)},
```

A walk that ends with no FDE is whole only if its pc is one of these and its sp is that far below the top:

```cpp
bool const complete = step == unwind::StepResult::End ||
                      (step == unwind::StepResult::NoFde && is_stack_root(regs, context));
```

Both conditions matter. The depth alone is not enough: the two frame sizes differ by 32 bytes, so a mid-stack routine in one path could sit at the other path's depth. The return address alone is not enough either, since the exception stub is also entered from kernel mode, deeper in the stack. Kernel threads still end at `Thread::trampoline`, whose null return address is an End, and an application processor's first frame has a null return address too.

The failure is safe in one direction. A root that is missing or mismeasured marks whole stacks as prefixes; nothing can mark a prefix whole.

Rejected:

- Emitting the assembly CFI into `.eh_frame`, so that walks go through user copies and nested exceptions. It needs the parser to accept the 'S' augmentation (`docs/repairs/261005/unwind.md` item 1). And `interrupt_common`'s CFI gives the interrupted sp as the CFA, which is 8 bytes off whenever the CPU realigned the stack, so walks through it could record wrong frames. That would be more complete but less reliable; it can come later.
- Judging by depth alone, which is ambiguous as said above.

**Reference**
None for the mechanism: the upstream sampler does not sample kernel stacks. The idea that the code which starts a stack declares its end is the role of `.cfi_undefined rip` in a program's `_start`, which DWARF unwinders take as the outermost frame.

**Verification**
Host: prof sampler tests 23/23 (one new: a root at its depth is whole; other code at that depth, and the root's return address deeper down, are prefixes), format 4/4, unwind 10/10, cfi 11/11. `boot_check.py --out out/sampler`, three times: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required). Across those runs every kernel record ending at `x86_syscall` (22) was unflagged, so the 144-byte root matched each time. Records ending at `Thread::trampoline` (97) stayed unflagged, and CPU 0's idle records (38) stayed flagged, as before (`docs/repairs/261005/kernel-task.md`). The exception root is checked by construction rather than by a sample, since none of the three runs caught one: `x86_exception_return` is at `interrupt_common+0x56`, the return address the earlier captures showed for that path, and the stub has pushed 7 words plus 15 registers, 176 bytes, when it calls.

**Deliberately not done**
Walking through assembly; see Rejected.

**How it was found**
The audit of 2026-10-05; the captures showed the one-frame `x86_user_copy` records.

**The user's prediction**
None. The user asked for whatever is safe and reliable; this was chosen on that ground.

**Three questions you should be able to answer**
- Why does a frame with no FDE not mean the stack has ended, in this kernel?
- Why must both the return address and the depth match a root, and what goes wrong with each one alone?
- Which way does this check fail when a root is wrong, and why is that the safe way?
