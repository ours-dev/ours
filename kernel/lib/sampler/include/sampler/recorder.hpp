/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SAMPLER_RECORDER_HPP
#define SAMPLER_RECORDER_HPP 1

#include <sampler/buffer.hpp>
#include <sampler/format.hpp>
#include <sampler/sink.hpp>

#include <span>

#include <ours/status.hpp>

namespace sampler {

/// Whose a sample is, and when it was taken: what a record stores beside
/// its stack.
///
/// Every field is a fact the kernel already owns, so the kernel fills them
/// all in from its own state. This library asks nothing of the platform and
/// calls nothing to find them out, which is what keeps it free of an arch
/// seam -- it used to read the CPU and the time through one, and the
/// architecture code that implemented it ended up calling back into the
/// recorder from its interrupt handler.
struct SampleContext final {
    u32 tid;
    /// The koid of the interrupted thread's process; 0 for a kernel thread.
    u64 pid;
    /// Picks the buffer the record goes to, and is stored in it.
    u32 cpu;
    /// Monotonic nanoseconds; used only to order samples for display.
    u64 timestamp;
};

/// One profiling session's recorder: where samples are stored, and the dump
/// they are read back as. It neither takes a sample nor walks a stack: the
/// caller hands it a stack already turned into addresses -- a kernel stack
/// by the KernelUnwinder, a user stack by walk_frame_pointers() -- with the
/// flags that say how. The kernel keeps one per session, inside the session
/// that owns its buffers (sampler::Session), so that a session's records can
/// outlive the next session's creation.
///
/// `Recorder` owns no dynamic storage and makes no calls into
/// `ours::`/`gktl::`/`logz4::`: the per-CPU buffers come in through
/// `Resources`, each sample's facts through `SampleContext`, and the dump
/// goes out through a writer the caller supplies (`ByteWriter` or
/// `LineWriter`).
///
/// It has no running state of its own. When it may be written and when read
/// is its owner's to say, since only the owner knows where its producers run:
/// a producer writes only while the owner lets producers in, and reset() and
/// the reads run only while it keeps them out. The Session does both with
/// one atomic, ACTIVE, and the IPI barrier its stop sends.
class Recorder final {
  public:
    using Sink = RecordSink<SampleRecord>;

    /// Looks up the per-CPU sample arena for `cpu`; returns nullptr when
    /// none exists (e.g. `cpu` is out of range). The caller keeps owning
    /// the buffers' actual storage — mirrors `unwind::MemoryReader`'s
    /// context-callback shape.
    using BufferLookup = Sink::BufferLookup;

    /// The buffers, owned and kept alive by the caller. `Recorder` allocates
    /// nothing itself.
    struct Resources final {
        BufferLookup buffer_lookup = nullptr;
        void *buffer_lookup_context = nullptr;
        u32 num_cpus = 0;
    };

    Recorder() = default;
    Recorder(Recorder const &) = delete;
    auto operator=(Recorder const &) -> Recorder & = delete;

    /// Wires up `resources` and resets every buffer they reach. Called once,
    /// before the first run. Whether any buffer resolves: the caller's own
    /// allocation can have failed, leaving a valid lookup that yields nothing.
    [[nodiscard]]
    auto init(Resources const &resources) -> bool;

    /// Empties every buffer, for a new run. Only while no producer can
    /// write: the owner publishes the emptied buffers to the producers it
    /// lets in afterwards.
    auto reset() -> void;

    /// Stores one sample in |context|'s CPU's buffer: |ips|, the stack from
    /// the interrupted pc outwards, at most `MAX_STACK_DEPTH` of them kept,
    /// with |flags|. Nothing for an empty |ips|. Allocates nothing and does
    /// bounded work, so it runs in the sampling timer's interrupt; only while
    /// the owner lets producers in, from one producer per CPU at a time,
    /// with interrupts off.
    auto record(SampleContext const &context, std::span<u64 const> ips, RecordFlags flags) -> void;

    /// Counts a tick on |cpu| that the caller records nothing for, because it
    /// landed where no target was asked for; the dump header sums them.
    /// Under the same rule as record().
    auto note_unmatched(u32 cpu) -> void;

    [[nodiscard]]
    auto num_cpus() const -> u32;

    /// Per-CPU buffer statistics; stable once sampling has stopped.
    [[nodiscard]]
    auto buffer_stats(u32 cpu) const -> BufferStats;

    /// Encodes the dump header and every retained record as hex text (the
    /// `PROF1`/`SAMP` line framing documented in `sampler/format.hpp`) and
    /// hands each line to |writer|. Carries no I/O policy of its own.
    /// |load_base| goes into the header: where the kernel image the kernel
    /// records' addresses lie in was loaded.
    auto export_dump(LineWriter const &writer, u64 load_base) const -> void;

    /// Where copy_dump() puts the dump's bytes: consecutive pieces of it, at
    /// increasing offsets from 0. A non-Ok answer stops the copy, and
    /// copy_dump() returns it.
    struct ByteWriter final {
        void *context;
        auto (*write)(void *context, usize offset, void const *bytes, usize len) -> ours::Status;
    };

    /// How many bytes copy_dump() writes: one DumpHeader, then every
    /// retained record. Stable once sampling has stopped.
    [[nodiscard]]
    auto dump_size() const -> usize;

    /// The same dump as export_dump(), as raw bytes instead of hex lines:
    /// the header, then every CPU's retained records in CPU order, exactly
    /// dump_size() bytes when it succeeds. For a reader that parses the
    /// structures itself rather than a console that only carries text.
    [[nodiscard]]
    auto copy_dump(ByteWriter const &writer, u64 load_base) const -> ours::Status;

  private:
    /// The header both forms of the dump start with.
    [[nodiscard]]
    auto make_header(u64 load_base) const -> DumpHeader;

    /// Where records land. Everything about *storing* a record lives here;
    /// `Recorder` decides what goes in it.
    Sink m_sink;
};

} // namespace sampler

#endif // #ifndef SAMPLER_RECORDER_HPP
