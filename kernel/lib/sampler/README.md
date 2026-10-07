# sampler

The kernel's sampling profiler, in two halves.

- `:core` turns a stack into addresses and stores them. `KernelUnwinder` unwinds an interrupted kernel frame with the `.eh_frame` CFI (libunwind); `walk_frame_pointers()` walks a user stack by its frame pointers; `Recorder` stores either as a raw, un-symbolized record in a pre-allocated per-CPU buffer and reads them back as the dump. It builds in the host toolchain, where its tests and the `prof-fold` tool run.
- `:sampler` is when a sample is taken and what a session owns. `sampler::Session` arms a timer on every CPU while a session runs, turns each interrupted stack into addresses with the core, and hands them to its `Recorder`; nothing samples, and no tick pays for the profiler, while no session runs. It is built on the kernel's timers, IPIs and locks, so it is kernel-only.

The core is standalone: it makes no calls into `ours::`, `gktl::` or `logz4::`, allocates nothing itself, and asks the platform nothing. Every fact a sample needs arrives as an argument: the registers and stack bounds in a `KernelFrame`, the thread, process, CPU and time in a `SampleContext`. Every kernel-owned resource is handed in: the per-CPU buffers through `Recorder::init`, the unwind FDE index and the stack roots through `KernelUnwinder::init`. The kernel image's load base goes to the dump calls, and the dump leaves through a caller-supplied writer: raw bytes through a `ByteWriter` (`Recorder::copy_dump`), or hex lines through a `LineWriter` (`Recorder::export_dump`). There is no arch seam: the recorder used to read the CPU, the time and the console through one, and the x86 code that implemented it ended up calling back into it from its interrupt handler.

The caller is `sampler::Session`, one per profiling session: it owns a `KernelUnwinder`, a `Recorder`, the `CpuLocal` buffers and the FDE index built from the kernel's own `.eh_frame`, all allocated when the session is created. `SamplerDispatcher` (`kernel/object`) gives one to user space through `sampler_create`, `_start`, `_stop` and `_read`, which copies `Recorder::copy_dump()`'s raw bytes out. `examples/profile` is a program that drives one.

## Scope

| Header | Contents |
|---|---|
| `sampler/format.hpp` | The core's names for the dump's structures, which `sampler_read` hands programs and so are defined in `<ours/sampler.hpp>`: `SampleRecord` (160 B), `DumpHeader` (40 B); and the `PROF1`/`SAMP` text tags |
| `sampler/record_buffer.hpp` | `RecordBuffer<Record>`: a per-CPU arena of fixed-size records over slots the caller attaches, with one atomic write cursor |
| `sampler/sink.hpp` | `RecordSink<Record>`: the per-CPU buffers behind a lookup the caller supplies, their stats, and the hex-line export through a `LineWriter` |
| `sampler/buffer.hpp` | `SampleBuffer = RecordBuffer<SampleRecord>` |
| `sampler/kernel_unwinder.hpp` | `KernelUnwinder`: an interrupted kernel frame (`KernelFrame`) to its stack's addresses, with `Truncated` when they are a prefix; `StackRoot` |
| `sampler/frame_pointers.hpp` | `walk_frame_pointers()`: a user stack by its frame pointers, through a reader the caller supplies |
| `sampler/recorder.hpp` | `Recorder`: storing a stack's addresses and reading the records back as the dump; `SampleContext`, whose a sample is |
| `sampler/stack_window.hpp` | `StackWindow`: the stretch of the interrupted stack, from sp to its top, that the kernel unwind may read |
| `sampler/session.hpp` | `Session`, kernel-only: the system's one session, its timers, buffers and unwind index |
| `sampler/stack_sample_request.hpp` | `StackSampleRequest`: what a sampling tick leaves on a thread, held by the thread by value |

## Shape

- **Inputs.** A `KernelFrame` carries the interrupted pc, sp and frame pointer and the bounds of the stack they ran on; a `SampleContext` the thread, process, CPU and monotonic time. The session fills them in from the interrupt frame (`arch::IrqFrame::pc()` and friends, the only architecture-specific part) and from the kernel's own state.
- **Interrupt-safe.** `KernelUnwinder::unwind` and `Recorder::record` allocate nothing and do bounded work, so both run in the sampling timer's interrupt.
- **Unwinding.** `KernelUnwinder::unwind` walks at most 16 frames with `unwind::Unwinder` over the kernel's `.eh_frame` (kept in the loaded image by kernel.ld; see `kernel_unwind_tables_enabled`). Every read goes through a `StackWindow`, from the interrupted sp to the top of the stack the kernel says it is on (`KernelFrame::stack_base`/`stack_top`, the running thread's kernel stack), so a corrupt frame cannot become a read past the stack; a sample whose sp is not on that stack keeps its pc alone and is marked `Truncated`. User stacks are walked by frame pointers (`walk_frame_pointers()`) on the thread's way back to user space. Either stack is stored by `Recorder::record()`, with `FromUser` or `Truncated` as the walk says. The section bytes and the sorted FDE-entry array are supplied to `KernelUnwinder::init` by the session, which scans `.eh_frame` and owns the array behind them; the unwinder never allocates to build the index, mirroring `unwind::FdeIndex`'s own allocation-free contract.
- **Buffers.** `Recorder::init`'s `Resources::buffer_lookup` is a callback the caller supplies (`SampleBuffer *(*)(void *context, u32 cpu)`), mirroring `unwind::MemoryReader`'s context-callback shape. The recorder never allocates a buffer itself, nor its slots: a buffer's depth is whatever slots its owner attaches (`RecordBuffer::attach()`). `Session` backs the callback with `ours::mm::CpuLocal::allocate<SampleBuffer>` and gives every possible CPU the slots its creator asked for, from one allocation. A CPU's buffer has one producer at a time, always with interrupts off on that CPU: the sampling interrupt for a kernel record, and for a user record the thread writing its walked stack (wait-free cursor). The recorder has no running state of its own: `Session::stop()` clears `ACTIVE`, which every producer checks with interrupts off, then waits out any producer in flight with an IPI to every CPU, so the consumer never reads a record torn.
- **Control.** A program holding the SAMPLING resource calls `ours::sampler_create`, `_start`, `_stop` and `_read` (`<ours/vdso.hpp>`, with the configuration and the dump's structures in `<ours/sampler.hpp>`); closing the handle ends the session. The data plane has one atomic, `Session::ACTIVE`: `Session::start()` empties the buffers (`Recorder::reset()`) while no producer can reach them, then publishes them with the release store of `ACTIVE`; each producer acquires `ACTIVE` and writes only while it names its session. The control plane (create, start, stop, release, read) is serialised by `Session`'s mutex.

## Export pipeline

```text
sampler_read  →  the program's buffer: a SamplerDumpHeader, then records
examples/profile  →  the console, as text lines:
  PROFP <koid>                       (whose code the markup describes)
  {{{reset}}} {{{module:...}}} {{{mmap:...}}}
                                     (its build ID and loaded segments,
                                      LLVM symbolizer markup)
  PROF1 <40-byte header, hex>        (once; carries the runtime load base)
  SAMPB <160-byte record, base64>    (per sample; one debug_write each)
           ↓ host
prof-fold --elf vmours --samples capture.txt
          --debug-json out/<dir>/kernel.dev.debug.json > folded.txt
           ↓
flamegraph.pl folded.txt > flame.svg
```

`Recorder::copy_dump` produces the raw bytes `sampler_read` copies out. `Recorder::export_dump` builds the same dump as `PROF1`/`SAMP` hex lines for a caller with a console of its own, handing each line to a `LineWriter` in one call; `prof-fold` (`cli/`) reads both kinds of record line. No I/O policy lives in the core itself.

`prof-fold` resolves record IPs against the kernel ELF symbol table after subtracting `DumpHeader.load_base` (the image is 0-linked PIC), demangles C++ names, and folds per `(pid, tid, call-stack)`. A user frame is resolved against its own program's unstripped ELF: the process's markup gives the mmap that holds it and the module's build ID, and `--debug-json`, the list `gn gen` writes beside each image, maps the build ID to the ELF. A process that printed no markup, or a module the list does not name, keeps its frames as addresses.

## Tests

`//kernel/lib/sampler:tests` runs on the host toolchain:

- `test/format_test.cpp` pins the dump's layout: record and header sizes and offsets, magic values, and a hex round-trip. This is the contract the host parser depends on.
- `test/recorder_test.cpp` exercises the `Recorder` itself: buffer depth and selection by the context's CPU, both dump forms with the load base they are given, and the flags a record keeps.
- `test/stacks_test.cpp` exercises how a stack becomes addresses: the `KernelUnwinder`'s verdict on where a kernel stack ends (a stack root, a frame off its stack), `StackWindow`'s bounds, and the frame-pointer walk over a fake reader.

Together they are the proof that the core needs nothing from `ours::`, `gktl::`, `logz4::`, `CpuLocal` or an architecture to build and run.

The session has no host tests. The shell's sampler checks and `examples/profile` exercise it on both images.
