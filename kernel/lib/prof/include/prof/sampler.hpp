/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_SAMPLER_HPP
#define PROF_SAMPLER_HPP 1

#include <atomic>

#include <prof/buffer.hpp>
#include <prof/format.hpp>
#include <prof/sink.hpp>
#include <unwind/cfi.hpp>
#include <unwind/fde_index.hpp>

namespace ours {
enum class Status : u32;
}

namespace prof {

/// Where sampling interrupts come from.
enum class Source : u8 {
    /// PMU counter overflow (unhalted core cycles); fails when the machine
    /// has no architectural PMU (e.g. QEMU TCG).
    Pmu,
    /// Sample on scheduler ticks via the timekeeping subsystem; always
    /// available, coarser (OURS_CONFIG_HZ).
    TimerTick,
};

struct Config final {
    Source source;
    /// For `Pmu`: counter counts between interrupts. For `TimerTick`: take
    /// a sample every N-th scheduler tick.
    u64 period;
};

/// Captured by the sampling interrupt. `arch::` fills this in from the
/// interrupted CPU's trap frame, keeping this library arch-neutral.
struct SampleContext final {
    u64 pc;
    u64 sp;
    u64 bp;
    u32 tid;
    bool from_uspace;
};

/// The kernel's sole sampling profiler. `Sampler` owns no dynamic storage
/// and makes no calls into `ours::`/`gktl::`/`logz4::` — every kernel-side
/// resource (per-CPU buffers, the unwind tables, the current CPU/timestamp/
/// diagnostic output) is either injected through `Resources` or read
/// through the `prof::arch` seam. Booting the profiler, wiring it to a
/// console, and picking where its buffers live is the caller's job (see
/// `kernel/kernel/prof`).
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

    static auto get() -> Sampler &;

    /// Boot-time bring-up: wires up `resources` and resets every buffer
    /// they reach. Safe to call before any CPU has taken an interrupt;
    /// failures are reported through the returned flags, never fatal.
    auto init(Resources const &resources) -> InitFlags;

    [[nodiscard]]
    auto start(Config const &config) -> ours::Status;

    /// Disable sampling and quiesce every CPU's sampler so buffers can be
    /// read.
    auto stop() -> void;

    [[nodiscard]]
    auto is_running() const -> bool;

    [[nodiscard]]
    auto active_source() const -> Source;

    /// Current sampling period (events per interrupt, or ticks-divisor).
    [[nodiscard]]
    auto active_period() const -> u64;

    /// Take one sample now. Runs in interrupt context: no allocation,
    /// bounded work, unwinds at most `MAX_STACK_DEPTH` frames.
    auto record_sample(SampleContext const &context) -> void;

    /// Timer-fallback hook, called from the scheduler tick. No-op unless
    /// the `TimerTick` source is running; samples on every `period`-th
    /// tick.
    auto on_scheduler_tick() -> void;

    [[nodiscard]]
    auto num_cpus() const -> u32;

    /// Per-CPU buffer statistics; stable once sampling has stopped.
    [[nodiscard]]
    auto buffer_stats(u32 cpu) const -> BufferStats;

    /// Encodes the dump header and every retained record as hex text (the
    /// `PROF1`/`SAMP` line framing documented in `prof/format.hpp`) and
    /// hands each line to `arch::debug_write`. Carries no I/O policy of
    /// its own — the seam owns that.
    auto export_dump() const -> void;

  private:
    Sampler() = default;
    Sampler(Sampler const &) = delete;
    auto operator=(Sampler const &) -> Sampler & = delete;

    enum class State : u8 {
        Stopped,
        /// Claimed by a starter that has not published its config yet. No
        /// reader acts on it, which is what lets the config be filled in
        /// before it becomes visible as `Running`.
        Starting,
        Running,
        /// Set while stopping; interrupt handlers may still see it mid-flight.
        Stopping,
    };

    auto unwind_context(SampleContext const &context, SampleRecord *record) const -> void;

    /// The one atomic. `m_source` and `m_period` are plain fields published
    /// through it, so they never need to be atomic themselves and the three
    /// never need to be packed into one word to stay coherent:
    ///
    ///   - `start()` claims the sampler with a Stopped -> Starting CAS.
    ///     Starting is a state no reader acts on, so the two fields can be
    ///     written in the open with nothing able to observe them half-set.
    ///   - The Running store is a release; it publishes both fields.
    ///   - Every reader loads the state with acquire and touches the fields
    ///     only after seeing Running, so it sees the values written before
    ///     that release -- the same publish protocol as Linux pairing
    ///     `smp_store_release()` after initialisation with
    ///     `smp_load_acquire()` on the reader side
    ///     (Documentation/memory-barriers.txt, "RELEASE/ACQUIRE").
    ///
    /// What the protocol does not cover, on purpose: a sampling interrupt on
    /// another CPU that saw Running and was then held off across a whole
    /// stop()/start() cycle before reading the fields. That is a console
    /// round trip against a handler that runs for nanoseconds, and both
    /// fields are naturally aligned words on every supported target, so the
    /// worst case is one sample taken with the neighbouring run's period.
    std::atomic<State> m_state{State::Stopped};
    Source m_source{Source::Pmu};
    u64 m_period{0};
    /// Not part of the configuration: a free-running counter the tick source
    /// advances, read only while `Running`.
    std::atomic<u64> m_tick_counter{0};

    /// Where records land. Everything about *storing* a record lives here
    /// and is shared with any other producer; `Sampler` only decides when a
    /// record happens and what goes in it.
    Sink m_sink;

    unwind::Section m_eh_section{};
    unwind::FdeIndex m_fde_index;
    u64 m_image_base = 0;
};

[[nodiscard]]
constexpr auto operator|(Sampler::InitFlags lhs, Sampler::InitFlags rhs) -> Sampler::InitFlags {
    return static_cast<Sampler::InitFlags>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

[[nodiscard]]
constexpr auto operator&(Sampler::InitFlags lhs, Sampler::InitFlags rhs) -> Sampler::InitFlags {
    return static_cast<Sampler::InitFlags>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

[[nodiscard]]
constexpr auto has_flag(Sampler::InitFlags flags, Sampler::InitFlags bit) -> bool {
    return (flags & bit) != Sampler::InitFlags::None;
}

} // namespace prof

#endif // #ifndef PROF_SAMPLER_HPP
