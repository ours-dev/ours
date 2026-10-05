# libprof

The core of the kernel's sampling profiler: what a sample contains and
where it is stored. Given one interrupted context, `Sampler::record_sample`
unwinds the kernel call stack with `.eh_frame` CFI (libunwind) and stores a
raw, un-symbolized record into a pre-allocated per-CPU buffer. A text
export plus the host `prof-fold` tool turn the records into flamegraph.pl
folded stacks.

When a sample is taken is not this library's business. `ours::sampler`
(`kernel/kernel/sampler`) arms a timer on every CPU while a session runs
and hands each interrupted context in; nothing samples, and no tick pays
for the profiler, while no session runs.

This library is standalone: it makes no calls into `ours::`, `gktl::`, or
`logz4::`, allocates nothing itself, and asks the platform nothing. Every
fact a sample needs (registers, CPU, thread, time) arrives in its
`SampleContext`, every kernel-owned resource (per-CPU buffers, the unwind
FDE index) is handed in through `Sampler::init`, and the dump leaves
through a caller-supplied `LineWriter`. There is no arch seam: it used to
read the CPU, the time and the console through one, and the x86 code that
implemented it ended up calling back into `Sampler` from its interrupt
handler. `kernel/commands/prof.cpp` is the caller today: it owns the
`CpuLocal` buffers, builds the FDE index from the kernel's own `.eh_frame`,
and exposes `prof.start`/`stop`/`dump` as console commands.

## Scope

| Header | Contents |
|---|---|
| `prof/format.hpp` | The wire format shared with the host tool: `SampleRecord` (152 B) + `DumpHeader` (32 B) |
| `prof/record_buffer.hpp` | `RecordBuffer<Record>`: per-CPU arena of any fixed-size record over slots the caller attaches, one atomic write cursor |
| `prof/buffer.hpp` | `SampleBuffer = RecordBuffer<SampleRecord>` |
| `prof/sink.hpp` | `RecordSink<Record>`: the storage half shared by a sampler and a tracer — injected per-CPU buffers, stats, hex-line export through a `LineWriter` |
| `prof/sampler.hpp` | `Sampler`: the profiler's state, recording and export; `SampleContext`, what the kernel hands in |
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
  image by kernel.ld; see `kernel_unwind_tables_enabled`). Reads are gated
  to the interrupted SP's 64 KiB stack window. Samples from user space are
  recorded depth-1 with `RecordFromUspace` — no user unwind yet. The
  section bytes and the sorted FDE-entry array are supplied to
  `Sampler::init` by the caller (`kernel/commands/prof.cpp` scans
  `.eh_frame` and owns the page-backed array behind them) — `Sampler`
  itself never allocates to build the index, mirroring
  `unwind::FdeIndex`'s own allocation-free contract.
- **Buffers.** `Sampler::init`'s `Resources::buffer_lookup` is a callback
  the caller supplies (`SampleBuffer *(*)(void *context, u32 cpu)`),
  mirroring `unwind::MemoryReader`'s context-callback shape. `Sampler`
  never allocates a buffer itself, nor its slots: a buffer's depth is
  whatever slots its owner attaches (`RecordBuffer::attach()`).
  `kernel/commands/prof.cpp` backs the callback with
  `ours::mm::CpuLocal::allocate<SampleBuffer>` and gives every possible
  CPU 128 slots from one allocation. The producer is the sampling interrupt alone
  (wait-free cursor). `Sampler::stop()` only stops accepting samples;
  `ours::sampler::stop()` then waits out any in flight with an IPI to
  every CPU, so the consumer never reads a record torn.
- **Control.** `prof.start [period-ns]`, `prof.stop`, `prof.dump` are gktl
  commands defined in `kernel/commands/prof.cpp` — drive them over the
  kernel shell (libkshell) or from `scripts/kshell_run.py`. They go
  through `ours::sampler::start()`/`stop()`, which own the timers, and
  read the results from `prof::Sampler::get()`. Inside `Sampler` the only
  atomic is the state; `start()` empties the buffers while the state is
  `Starting` (which no producer acts on) and publishes them with the
  release store of `Running`.

## Export pipeline

```text
prof.dump  →  the kernel console, as text lines:
  PROF1 <32-byte header, hex>        (once; carries the runtime load base)
  SAMP <152-byte record, hex>        (per sample)
           ↓ host
prof-fold --elf vmours --samples capture.txt > folded.txt
           ↓
flamegraph.pl folded.txt > flame.svg
```

`Sampler::export_dump` builds each line (tag + hex + `\r\n`) on the stack
and hands it to the caller's `LineWriter` in one call — no I/O policy
lives in the library itself. `prof-fold` resolves record IPs against the
kernel ELF symbol table after subtracting `DumpHeader.load_base` (the image
is 0-linked PIC), demangles C++ names, and folds per `(tid, call-stack)`.

## Tests

- `//kernel/lib/prof:tests` runs on the host toolchain:
  - `tests/format_test.cpp` pins the wire format: record and header
    sizes/offsets, magic values, and a hex round-trip — the contract the
    host parser depends on.
  - `tests/sampler_test.cpp` exercises `Sampler` itself (state machine,
    buffer selection by the context's CPU, dump encoding) with no kernel
    and nothing faked underneath — proof that the library needs nothing
    from `ours::`/`gktl::`/`logz4::`/`CpuLocal` or an architecture to
    build and run.
