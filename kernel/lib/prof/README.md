# libprof

A perf-style sampling profiler for the kernel: PMU counter overflow (or a
scheduler-tick fallback) raises an interrupt; the handler captures the
interrupted context, unwinds the kernel call stack with `.eh_frame` CFI
(libunwind) and stores raw, un-symbolized samples into pre-allocated
per-CPU buffers. A serial export plus the host `prof-fold` tool turn the
samples into flamegraph.pl folded stacks.

This library is standalone: it makes no calls into `ours::`, `gktl::`, or
`logz4::`, and allocates nothing itself. Every platform fact it needs (PMU
control, the current CPU, a timestamp, a place to write diagnostic bytes)
is read through the `prof::arch` seam, and every kernel-owned resource
(per-CPU buffers, the unwind FDE index) is handed in by the caller through
`Sampler::init`. `kernel/kernel/prof` is that caller today — it owns the
`CpuLocal` buffers, builds the FDE index from the kernel's own `.eh_frame`,
and exposes `prof.start`/`stop`/`dump` as console commands. A future
`ThreadSampler` in the `object` module can be a second such caller without
this library changing at all.

## Scope

| Header | Contents |
|---|---|
| `prof/format.hpp` | The wire format shared with the host tool: `SampleRecord` (152 B) + `DumpHeader` (32 B) |
| `prof/buffer.hpp` | `SampleBuffer`: per-CPU fixed arena, atomic write cursor, drop counting |
| `prof/sampler.hpp` | `Sampler`: the profiler itself — state, control, recording, export |
| `prof/arch.hpp` | The arch seam (implemented once per architecture) |

## Shape

- **Sources.** `Source::Pmu` programs IA32_PERFEVTSEL0/PMC0 for
  UnHalted-Core-Cycles with the PMI bit and hands the overflow to
  `IrqVec::ApicPmi`. Machines without an architectural PMU (QEMU TCG)
  fail detection, and `Source::TimerTick` samples every N-th scheduler
  tick via `Sampler::on_scheduler_tick()` instead — same record path.
- **Interrupt-safe sampling.** The PMI handler halts the counter first so
  it never samples itself, records with zero allocation and bounded work,
  then re-arms. Tick samples recover the interrupted context through
  `arch::capture_sample_context()` (per-CPU, set by the IRQ preamble).
- **Unwinding.** `Sampler::record_sample` walks at most 16 frames with
  `unwind::Unwinder` over the kernel's `.eh_frame` (kept in the loaded
  image by kernel.ld; see `kernel_unwind_tables_enabled`). Reads are gated
  to the interrupted SP's 64 KiB stack window. Samples from user space are
  recorded depth-1 with `RecordFromUspace` — no user unwind yet. The
  section bytes and the sorted FDE-entry array are supplied to
  `Sampler::init` by the caller (`kernel/kernel/prof` scans `.eh_frame` and
  owns the `ktl::Vec` backing them) — `Sampler` itself never allocates to
  build the index, mirroring `unwind::FdeIndex`'s own allocation-free
  contract.
- **Buffers.** `Sampler::init`'s `Resources::buffer_lookup` is a callback
  the caller supplies (`SampleBuffer *(*)(void *context, u32 cpu)`),
  mirroring `unwind::MemoryReader`'s context-callback shape. `Sampler`
  never allocates a buffer itself; `kernel/kernel/prof` backs the callback
  with `ours::mm::CpuLocal::allocate<SampleBuffer>`, giving every CPU a
  128-record arena. The producer is the sampling interrupt alone (wait-free
  cursor); the consumer only runs after `Sampler::stop()` has quiesced
  every CPU (each acknowledges by bumping a counter in its stop IPI), so
  records are never read torn.
- **Control.** `prof.start [pmu|tick] [period]`, `prof.stop`, `prof.dump`
  are gktl commands defined in `kernel/kernel/prof` — drive them
  interactively over the serial console (libkshell) or from a script. They
  are thin wrappers over `prof::Sampler::get()`.

## The arch seam

`prof/arch.hpp` is the one place platform-specific behavior lives; the
kernel links exactly one implementation (x86: `kernel/arch/x86/kernel/
prof_pmu.cpp`):

| Function | Used for |
|---|---|
| `pmu_present` / `start_pmu_sampling` / `stop_pmu_sampling` | PMU-based sampling control |
| `capture_sample_context` | Recovering the interrupted context for the timer-tick fallback |
| `current_cpu` | Picking a sample's buffer and tagging its `SampleRecord::cpu` |
| `timestamp` | `SampleRecord::tsc` |
| `debug_write` | The raw bytes `Sampler::export_dump` produces (COM1 on x86) |

## Export pipeline

```
prof.dump  →  COM1 as text lines:
  PROF1 <32-byte header, hex>        (once; carries the runtime load base)
  SAMP <152-byte record, hex>        (per sample)
           ↓ host
prof-fold --elf vmours --samples capture.txt > folded.txt
           ↓
flamegraph.pl folded.txt > flame.svg
```

`Sampler::export_dump` builds each line (tag + hex + `\r\n`) on the stack
and hands it to `arch::debug_write` in one call — no I/O policy lives in
the library itself. `prof-fold` resolves record IPs against the kernel ELF
symbol table after subtracting `DumpHeader.load_base` (the image is
0-linked PIC), demangles C++ names, and folds per `(tid, call-stack)`.

## Tests

- `//kernel/lib/prof:tests` runs on the host toolchain:
  - `tests/format_test.cpp` pins the wire format: record and header
    sizes/offsets, magic values, and a hex round-trip — the contract the
    host parser depends on.
  - `tests/sampler_test.cpp` exercises `Sampler` itself (state machine,
    buffer selection via the injected `BufferLookup`, dump encoding)
    against a fake `prof::arch` (`tests/fake_arch.cpp`) standing in for a
    real kernel — proof that the library needs nothing from `ours::`/
    `gktl::`/`logz4::`/`CpuLocal` to build and run.
