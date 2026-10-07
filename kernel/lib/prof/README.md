# libprof

What a sampler and a tracer have in common, header-only. The sampling profiler itself (its recorder, its dump and `prof-fold`) is `kernel/lib/thread_sampler`, which stores its records through the buffers here.

| Header | Contents |
|---|---|
| `prof/record_buffer.hpp` | `RecordBuffer<Record>`: a per-CPU arena of any fixed-size record over slots the caller attaches, with one atomic write cursor |
| `prof/sink.hpp` | `RecordSink<Record>`: the storage half shared by a sampler and a tracer (injected per-CPU buffers, stats, hex-line export through a `LineWriter`) |
| `prof/trace.hpp` | `Tracepoint`, `TraceScope` and `TraceRecord`: static trace sites. The `Tracer` that drives them is not written yet; `examples/trace_examples.cpp` stands in |

Nothing here allocates or calls into the kernel: storage, the CPU a record goes to and where exported lines go are all handed in by the caller.
