# libprof

The core of the kernel's sampling profiler: what a sample contains and
where it is stored. Given one interrupted context, `Sampler::record_sample`
unwinds the kernel call stack with `.eh_frame` CFI (libunwind) and stores a
raw, un-symbolized record into a pre-allocated per-CPU buffer. A text
export plus the host `prof-fold` tool turn the records into flamegraph.pl
folded stacks.

When a sample is taken is not this library's business. `sampler`
(`kernel/lib/thread_sampler`) arms a timer on every CPU while a session runs
and hands each interrupted context in; nothing samples, and no tick pays
for the profiler, while no session runs.

This library is standalone: it makes no calls into `ours::`, `gktl::`, or
`logz4::`, allocates nothing itself, and asks the platform nothing. Every
fact a sample needs (registers, CPU, thread, time) arrives in its
`SampleContext`, every kernel-owned resource (per-CPU buffers, the unwind
FDE index) is handed in through `Sampler::init`, and the dump leaves
through a caller-supplied writer: raw bytes through a `ByteWriter`
(`Sampler::copy_dump`), or hex lines through a `LineWriter`
(`Sampler::export_dump`). There is no arch seam: it used to
read the CPU, the time and the console through one, and the x86 code that
implemented it ended up calling back into `Sampler` from its interrupt
handler. The caller is `sampler::Session` (`kernel/lib/thread_sampler`),
one per profiling session: it owns a `Sampler`, the `CpuLocal` buffers
and the FDE index built from the kernel's own `.eh_frame`, all allocated
when the session is created. `SamplerDispatcher` (`kernel/object`) gives
one to user space through `sampler_create`, `_start`, `_stop` and
`_read`, which copies `Sampler::copy_dump()`'s raw bytes out.
`examples/profile` is a program that drives one.

## Scope

| Header | Contents |
|---|---|
| `prof/format.hpp` | This library's names for the dump's structures, which `sampler_read` hands programs and so are defined in `<ours/sampler.hpp>`: `SampleRecord` (152 B), `DumpHeader` (32 B); and the `PROF1`/`SAMP` text tags |
| `prof/record_buffer.hpp` | `RecordBuffer<Record>`: per-CPU arena of any fixed-size record over slots the caller attaches, one atomic write cursor |
| `prof/buffer.hpp` | `SampleBuffer = RecordBuffer<SampleRecord>` |
| `prof/sink.hpp` | `RecordSink<Record>`: the storage half shared by a sampler and a tracer — injected per-CPU buffers, stats, hex-line export through a `LineWriter` |
| `prof/sampler.hpp` | `Sampler`: the profiler's state, recording and export; `SampleContext`, what the kernel hands in |
| `prof/stack_window.hpp` | `StackWindow`: the stretch of the interrupted stack, from sp to its top, that the kernel unwind may read |
| `prof/trace.hpp` | `Tracepoint` / `TraceScope` / `TraceRecord`: static trace sites (the `Tracer` that drives them is not written yet; `examples/trace_examples.cpp` stands in) |

## Shape

- **Inputs.** `SampleContext` carries the interrupted pc, sp and frame
  pointer, whether the interrupt came from user mode, and the thread, CPU
  and monotonic time. The kernel fills it in from the interrupt frame
  (`arch::IrqFrame::pc()` and friends, the only architecture-specific
  part) and from its own state.
- **Interrupt-safe recording.** `record_sample` allocates nothing and
  does bounded work, so it can run in the sampling timer's interrupt.
- **Unwinding.** `Sampler::record_sample` walks at most 16 frames with
  `unwind::Unwinder` over the kernel's `.eh_frame` (kept in the loaded
  image by kernel.ld; see `kernel_unwind_tables_enabled`). Every read goes
  through a `StackWindow`, from the interrupted sp to the top of the stack
  the kernel says it is on (`SampleContext::stack_base`/`stack_top`, the
  running thread's kernel stack), so a corrupt frame cannot become a read
  past the stack; a sample whose sp is not on that stack keeps its pc
  alone and is marked `Truncated`. User stacks are walked by frame
  pointers (`walk_frame_pointers()`) on the thread's way back to user
  space and stored by `Sampler::record_user_stack()`. The
  section bytes and the sorted FDE-entry array are supplied to
  `Sampler::init` by the caller (`sampler::Session` scans
  `.eh_frame` and owns the array behind them) — `Sampler`
  itself never allocates to build the index, mirroring
  `unwind::FdeIndex`'s own allocation-free contract.
- **Buffers.** `Sampler::init`'s `Resources::buffer_lookup` is a callback
  the caller supplies (`SampleBuffer *(*)(void *context, u32 cpu)`),
  mirroring `unwind::MemoryReader`'s context-callback shape. `Sampler`
  never allocates a buffer itself, nor its slots: a buffer's depth is
  whatever slots its owner attaches (`RecordBuffer::attach()`).
  `sampler::Session` backs the callback with
  `ours::mm::CpuLocal::allocate<SampleBuffer>` and gives every possible
  CPU the slots its creator asked for, from one allocation. A CPU's buffer
  has one producer at a time, always with interrupts off on that CPU: the
  sampling interrupt for a kernel record, and for a user record the thread
  writing its walked stack (wait-free cursor). `Sampler::stop()` only
  stops accepting samples; `Session::stop()` then waits out any producer in
  flight with an IPI to every CPU, so the consumer never reads a record
  torn.
- **Control.** A program holding the SAMPLING resource calls
  `ours::sampler_create`, `_start`, `_stop` and `_read` (`<ours/vdso.hpp>`,
  with the configuration and the dump's structures in `<ours/sampler.hpp>`);
  closing the handle ends the session. Inside
  `Sampler` the only atomic is the state; `start()` empties the buffers while the state is
  `Starting` (which no producer acts on) and publishes them with the
  release store of `Running`.

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
prof-fold --elf vmours --samples capture.txt \
          --debug-json out/<dir>/kernel.dev.debug.json > folded.txt
           ↓
flamegraph.pl folded.txt > flame.svg
```

`Sampler::copy_dump` produces the raw bytes `sampler_read` copies out.
`Sampler::export_dump` builds the same dump as `PROF1`/`SAMP` hex lines
for a caller with a console of its own, handing each line to a
`LineWriter` in one call; `prof-fold` reads both kinds of record line. No
I/O policy lives in the library itself. `prof-fold` resolves record IPs against the
kernel ELF symbol table after subtracting `DumpHeader.load_base` (the image
is 0-linked PIC), demangles C++ names, and folds per `(pid, tid, call-stack)`.
A user frame is resolved against its own program's unstripped ELF: the
process's markup gives the mmap that holds it and the module's build ID,
and `--debug-json`, the list `gn gen` writes beside each image, maps the
build ID to the ELF. A process that printed no markup, or a module the list
does not name, keeps its frames as addresses.

## Tests

- `//kernel/lib/prof:tests` runs on the host toolchain:
  - `test/format_test.cpp` pins the dump's layout: record and header
    sizes/offsets, magic values, and a hex round-trip — the contract the
    host parser depends on.
  - `test/sampler_test.cpp` exercises `Sampler` itself (state machine,
    buffer depth and selection by the context's CPU, both dump forms, the
    user-stack record), the frame-pointer walk over a fake reader, and
    `StackWindow`'s bounds, with no kernel underneath — proof that the
    library needs nothing from `ours::`/`gktl::`/`logz4::`/`CpuLocal` or an
    architecture to build and run.
