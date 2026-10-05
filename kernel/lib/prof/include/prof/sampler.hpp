/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_SAMPLER_HPP
#define PROF_SAMPLER_HPP 1

#include <prof/buffer.hpp>
#include <prof/format.hpp>
#include <prof/sink.hpp>

#include <atomic>

#include <ours/status.hpp>
#include <unwind/cfi.hpp>
#include <unwind/fde_index.hpp>
#include <ustl/util/enum_bits.hpp>

namespace prof {

/// One interrupted context, as the kernel hands it to `record_sample()`.
///
/// Every field is a fact the kernel already owns, so the kernel fills them
/// all in: the registers from the interrupt frame it took, the CPU, thread
/// and time from its own state. This library asks nothing of the platform
/// and calls nothing to find these out, which is what keeps it free of an
/// arch seam -- it used to read the CPU and the time through one, and the
/// architecture code that implemented it ended up calling back into
/// `Sampler` from its interrupt handler.
struct SampleContext final {
    u64 pc;
    u64 sp;
    u64 bp;
    u32 tid;
    /// Picks the buffer the record goes to, and is stored in it.
    u32 cpu;
    /// Monotonic nanoseconds; used only to order samples for display.
    u64 timestamp;
    bool from_uspace;
};

/// The kernel's sole sampling profiler: what a sample contains and where it
/// is stored, never when one is taken. `Sampler` owns no dynamic storage and
/// makes no calls into `ours::`/`gktl::`/`logz4::`: the per-CPU buffers and
/// the unwind tables come in through `Resources`, each sample's facts come in
/// through `SampleContext`, and the dump goes out through a `LineWriter`.
/// Deciding when a sample is due -- arming the per-CPU timers -- is
/// `ours::sampler`'s (kernel/kernel/sampler).
class Sampler final {
  public:
    using Sink = RecordSink<SampleRecord, SampleBuffer::CAPACITY>;

    /// Looks up the per-CPU sample arena for `cpu`; returns nullptr when
    /// none exists (e.g. `cpu` is out of range). The caller keeps owning
    /// the buffers' actual storage — mirrors `unwind::MemoryReader`'s
    /// context-callback shape.
    using BufferLookup = Sink::BufferLookup;

    /// Everything `init()` needs, all owned and kept alive by the caller.
    /// `Sampler` allocates nothing itself.
    struct Resources final {
        BufferLookup buffer_lookup = nullptr;
        void *buffer_lookup_context = nullptr;
        u32 num_cpus = 0;

        /// The kernel's `.eh_frame`; an empty section (size 0) degrades
        /// unwinding to depth-1 records rather than failing.
        unwind::Section eh_section{};
        /// Ascending-`pc_begin`-sorted FDE entries built by the caller
        /// (mirrors `unwind::FdeIndex::init`'s own storage contract); must
        /// outlive the `Sampler`.
        unwind::FdeIndex::Entry const *fde_entries = nullptr;
        usize fde_entry_count = 0;

        /// Runtime load address of the kernel image, stamped into
        /// `DumpHeader::load_base` for the host tool to subtract.
        u64 image_base = 0;
    };

    enum class InitFlags : u32 {
        None = 0,
        BuffersReady = 1u << 0,
        UnwindReady = 1u << 1,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(InitFlags);

    static auto get() -> Sampler &;

    /// Boot-time bring-up: wires up `resources` and resets every buffer
    /// they reach. Safe to call before any CPU has taken an interrupt;
    /// failures are reported through the returned flags, never fatal.
    auto init(Resources const &resources) -> InitFlags;

    /// Empties every buffer and starts accepting samples. BadState if
    /// already running.
    [[nodiscard]]
    auto start() -> ours::Status;

    /// Stops accepting samples. A `record_sample()` that saw the sampler
    /// running just before may still be writing when this returns; waiting
    /// for it is the caller's, since only the caller knows where its
    /// producers run (ours::sampler::stop() does it with an IPI barrier).
    auto stop() -> void;

    [[nodiscard]]
    auto is_running() const -> bool;

    /// Take one sample now. Runs in interrupt context: no allocation,
    /// bounded work, unwinds at most `MAX_STACK_DEPTH` frames.
    auto record_sample(SampleContext const &context) -> void;

    [[nodiscard]]
    auto num_cpus() const -> u32;

    /// Per-CPU buffer statistics; stable once sampling has stopped.
    [[nodiscard]]
    auto buffer_stats(u32 cpu) const -> BufferStats;

    /// Encodes the dump header and every retained record as hex text (the
    /// `PROF1`/`SAMP` line framing documented in `prof/format.hpp`) and
    /// hands each line to |writer|. Carries no I/O policy of its own.
    auto export_dump(LineWriter const &writer) const -> void;

  private:
    Sampler() = default;
    Sampler(Sampler const &) = delete;
    auto operator=(Sampler const &) -> Sampler & = delete;

    enum class State : u8 {
        Stopped,
        /// Claimed by a starter that is still emptying the buffers. No
        /// producer acts on it, which is what lets the buffers be reset in
        /// the open before they become visible as `Running`.
        Starting,
        Running,
    };

    auto unwind_context(SampleContext const &context, SampleRecord *record) const -> void;

    /// The one atomic. The buffer reset `start()` does is published through
    /// it, so nothing else needs to be atomic or packed alongside it:
    ///
    ///   - `start()` claims the sampler with a Stopped -> Starting CAS.
    ///     Starting is a state no producer acts on, so the buffers can be
    ///     emptied in the open with no sample landing in a half-reset one.
    ///   - The Running store is a release; it publishes the reset.
    ///   - `record_sample()` loads the state with acquire and writes only
    ///     after seeing Running, so it sees the buffers as the reset left
    ///     them: everything written before a release store happens-before
    ///     whatever follows an acquire load that reads that store's value.
    std::atomic<State> m_state{State::Stopped};

    /// Where records land. Everything about *storing* a record lives here
    /// and is shared with any other producer; `Sampler` only decides when a
    /// record happens and what goes in it.
    Sink m_sink;

    unwind::Section m_eh_section{};
    unwind::FdeIndex m_fde_index;
    u64 m_image_base = 0;
};

/// Reads as "this flag is set". ustl's bitmask operators give `&` and a
/// `!` that answers "no bits at all", so the test is a double negation; the
/// name exists to keep that out of call sites.
[[nodiscard]]
constexpr auto has_flag(Sampler::InitFlags flags, Sampler::InitFlags bit) -> bool {
    return !!(flags & bit);
}

} // namespace prof

#endif // #ifndef PROF_SAMPLER_HPP
