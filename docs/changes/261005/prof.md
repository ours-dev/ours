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
