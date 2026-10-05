/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SAMPLER_MOD_HPP
#define OURS_SAMPLER_MOD_HPP 1

#include <ours/status.hpp>
#include <ours/syscall/time.hpp>

/// The kernel half of the sampling profiler: what makes a sample happen.
///
/// //kernel/lib/prof records a sample it is handed and knows nothing of the
/// kernel. This decides when one is taken, by keeping a timer armed on every
/// CPU while a session runs and on none otherwise, so a kernel nobody
/// profiles pays nothing for the profiler. The namespace is `sampler` rather
/// than `prof` because an `ours::prof` would hide the library's own `prof`
/// from every caller inside namespace ours.
namespace ours::sampler {
/// The shortest period a timer session may sample at. Taking a sample costs
/// a few microseconds, so a period near that would leave a CPU doing little
/// but sampling.
inline constexpr auto MIN_PERIOD = Microseconds(10);

/// Starts the profiler: every online CPU has its sampling timer armed, to
/// fire every |period|, before this returns. |period| is at least
/// MIN_PERIOD (InvArgs otherwise). BadState if a session is already running.
[[nodiscard]]
auto start(Duration period) -> Status;

/// Stops the profiler. When this returns no CPU is writing a sample, or ever
/// will again until the next start(), so the buffers may be read.
auto stop() -> void;
} // namespace ours::sampler

#endif // #ifndef OURS_SAMPLER_MOD_HPP
