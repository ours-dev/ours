/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_SAMPLER_HPP
#define OURS_SAMPLER_HPP 1

#include <ours/types.hpp>

#include <cstddef>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// What a sampling session records. The values are ABI and never change.
///
/// Kernel: on each tick that interrupts kernel code, the kernel call stack,
/// unwound in the interrupt. User: on each tick that interrupts user code,
/// the user call stack, walked on the thread's way back to user mode; not
/// accepted yet (NotSupported).
enum class SamplerTargets : u32 {
    None = 0,
    User = 1u << 0,
    Kernel = 1u << 1,
};
USTL_ENABLE_ENUM_BITMASK(SamplerTargets);

/// The shortest period a session may sample at, in nanoseconds. Taking a
/// sample costs a few microseconds, so a period near that would leave a CPU
/// doing little but sampling.
inline constexpr i64 SAMPLER_MIN_PERIOD = 10'000;

/// The most a session may keep per CPU, in bytes.
inline constexpr u64 SAMPLER_MAX_BUFFER_SIZE = u64{1} << 30;

/// sampler_create's configuration.
struct SamplerConfig {
    /// How often each CPU takes a sample, in nanoseconds; at least
    /// SAMPLER_MIN_PERIOD. The kernel's timers have the resolution of its
    /// tick, so a period shorter than a tick samples once a tick.
    i64 period;

    /// Bytes each CPU keeps, from one record to SAMPLER_MAX_BUFFER_SIZE;
    /// rounded down to whole records. A full buffer drops the samples that
    /// follow and counts them.
    u64 buffer_size;

    /// What to record; at least one target.
    SamplerTargets targets;

    /// Must be zero.
    u32 reserved;
};
static_assert(sizeof(SamplerConfig) == 24);
static_assert(offsetof(SamplerConfig, period) == 0);
static_assert(offsetof(SamplerConfig, buffer_size) == 8);
static_assert(offsetof(SamplerConfig, targets) == 16);
static_assert(offsetof(SamplerConfig, reserved) == 20);
} // namespace ours

#endif // #ifndef OURS_SAMPLER_HPP
