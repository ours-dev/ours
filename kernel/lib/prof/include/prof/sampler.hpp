/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_SAMPLER_HPP
#define PROF_SAMPLER_HPP 1

#include <prof/buffer.hpp>
#include <prof/format.hpp>
#include <ustl/sync/atomic.hpp>

#include <cstddef>
#include <cstdint>

namespace ours {
enum class Status : std::uint32_t;
}

namespace prof {

/// Where sampling interrupts come from.
enum class Source : std::uint8_t {
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
    std::uint64_t period;
};

/// Boot-time bring-up: builds the FDE index over the kernel's `.eh_frame`
/// and allocates the per-CPU sample buffers. Safe to call at `Platform`
/// init level; failures are logged, not fatal (profiling stays disabled).
auto init() -> void;

[[nodiscard]]
auto start(Config const &config) -> ours::Status;

/// Disable sampling and quiesce every CPU's sampler so buffers can be read.
auto stop() -> void;

[[nodiscard]]
auto is_running() -> bool;

[[nodiscard]]
auto active_source() -> Source;

/// Captured by the sampling interrupt. `arch::` fills this in from the
/// interrupted CPU's trap frame, keeping this library arch-neutral.
struct SampleContext final {
    std::uint64_t pc;
    std::uint64_t sp;
    std::uint64_t bp;
    std::uint32_t tid;
    bool from_uspace;
};

/// Take one sample now. Runs in interrupt context: no allocation, bounded
/// work, and it unwinds at most MAX_STACK_DEPTH frames.
auto record_sample(SampleContext const &context) -> void;

/// Timer-fallback hook, called from the scheduler tick. No-op unless the
/// TimerTick source is running; samples on every `period`-th tick.
auto on_scheduler_tick() -> void;

/// Log per-CPU buffer statistics over the console log.
auto log_summary() -> void;

/// Write the dump container (header line + `SAMP` hex lines) to COM1.
auto dump_serial() -> void;

} // namespace prof

#endif // #ifndef PROF_SAMPLER_HPP
