/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_OBJECT_INFO_HPP
#define OURS_OBJECT_INFO_HPP 1

#include <ours/types.hpp>

#include <cstddef>

#include <ustl/util/enum_bits.hpp>

namespace ours {
/// What ours::object_get_info is asked about. Each topic names one record
/// layout, which the kernel writes into the caller's buffer.
///
/// The values are ABI and never change, as ObjectType's. A topic carries its
/// layout version in bits 28..31: Process is topic 3 at version 1, the layout
/// with start_time and a flags word; version 0 had three bools in their place.
///
/// Only Process is answered. HandleBasic is named so that a caller can ask for
/// a topic the ABI numbers but this kernel does not answer (NotSupported); the
/// others are added with the code that answers them.
enum class InfoTopic : u32 {
    None = 0,
    HandleBasic = 2,
    Process = 3 | (1U << 28),
};

/// The state bits of an InfoProcess. Bit 2 is kept for a third, a debugger
/// being attached, left out while there is no debugger to attach.
enum class InfoProcessFlags : u32 {
    None = 0,

    /// process_start has run; start_time is valid.
    Started = 0_bit,

    /// The process has ended, by its own exit or by a kill; return_code is
    /// valid.
    Exited = 1_bit,
};
USTL_ENABLE_ENUM_BITMASK(InfoProcessFlags);

/// What object_get_info writes for InfoTopic::Process: one record.
///
/// A record crosses the ABI as bytes, so its layout is pinned below rather
/// than left to the compiler.
struct InfoProcess {
    /// What the process ended with: the value it passed to process_exit, or
    /// one of the TASK_RETCODE_* values if the kernel killed it. Valid only
    /// once flags has Exited.
    i64 return_code;

    /// When process_start ran, on the monotonic clock. Valid only once flags
    /// has Started.
    Instant start_time;

    InfoProcessFlags flags;

    /// Zero. Explicit, so that no byte the kernel copies out is left over
    /// from whatever its stack held before.
    u8 padding1[4];
};
static_assert(sizeof(InfoProcess) == 24);
static_assert(alignof(InfoProcess) == 8);
static_assert(offsetof(InfoProcess, return_code) == 0);
static_assert(offsetof(InfoProcess, start_time) == 8);
static_assert(offsetof(InfoProcess, flags) == 16);
static_assert(offsetof(InfoProcess, padding1) == 20);
} // namespace ours

#endif // #ifndef OURS_OBJECT_INFO_HPP
