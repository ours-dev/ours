/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SAMPLER_MOD_HPP
#define OURS_SAMPLER_MOD_HPP 1

#include <ours/status.hpp>
#include <ours/syscall/time.hpp>

#include <prof/sampler.hpp>

/// The kernel half of the sampling profiler: what makes a sample happen.
///
/// //kernel/lib/prof records a sample it is handed and knows nothing of the
/// kernel. This decides when one is taken, by keeping a timer armed on every
/// CPU while a session runs and on none otherwise -- Zircon's
/// sampler::ThreadSampler (zircon/kernel/lib/thread_sampler), whose per-CPU
/// sampling_timer is this timer. The namespace is `sampler` rather than
/// `prof` because an `ours::prof` would hide the library's own `prof` from
/// every caller inside namespace ours.
namespace ours::sampler {
/// The shortest period a timer session may sample at. Taking a sample costs
/// a few microseconds, so a period near that would leave a CPU doing little
/// but sampling. Zircon's ZX_SAMPLER_MIN_PERIOD, for the same reason.
inline constexpr auto MIN_PERIOD = Microseconds(10);

/// Starts the profiler with |config|. For prof::Source::Timer, |config|'s
/// period is in nanoseconds and at least MIN_PERIOD (InvArgs otherwise), and
/// every online CPU has its sampling timer armed before this returns.
///
/// BadState if a session is already running.
[[nodiscard]]
auto start(prof::Config const &config) -> Status;

/// Stops the profiler. When this returns no CPU is writing a sample, or ever
/// will again until the next start(), so the buffers may be read.
auto stop() -> void;
} // namespace ours::sampler

#endif // #ifndef OURS_SAMPLER_MOD_HPP
