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
/// the user call stack, walked by frame pointers on the thread's way back to
/// user mode. A tick that lands where no target asked records nothing.
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

/// The longest period a session may sample at, in nanoseconds: one second.
/// A slower rate takes too few samples to profile anything, and the bound
/// keeps the timers' deadline arithmetic (now plus a period) far from
/// overflowing, which a period near the type's maximum would.
inline constexpr i64 SAMPLER_MAX_PERIOD = 1'000'000'000;

/// The most a session may keep per CPU, in bytes: 4 MiB, about 27,000
/// records, minutes of samples at the kernel's tick. Every possible CPU gets
/// a buffer this size at create, so the bound times the CPU count is what
/// one create can take from the kernel's memory.
inline constexpr u64 SAMPLER_MAX_BUFFER_SIZE = u64{4} << 20;

/// sampler_create's configuration.
struct SamplerConfig {
    /// How often each CPU takes a sample, in nanoseconds; from
    /// SAMPLER_MIN_PERIOD to SAMPLER_MAX_PERIOD. A kernel whose timers fire at
    /// their own deadlines (kernel.time.oneshot) keeps the period; one on the
    /// periodic tick samples at most once a tick.
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

/// What sampler_read writes: one SamplerDumpHeader, then num_records
/// SamplerRecords, CPU by CPU. Addresses are not symbolised: a reader
/// resolves kernel ones against the kernel's ELF, after subtracting the
/// header's load_base.

/// The magic every record starts with: "SAMP" in memory order.
inline constexpr u32 SAMPLER_RECORD_MAGIC = 0x504D'4153;

/// The most frames one record holds.
inline constexpr usize SAMPLER_MAX_STACK_DEPTH = 16;

/// How a record was taken.
enum class SamplerRecordFlags : u8 {
    None = 0,
    /// The tick interrupted user code: ips are the user stack, the
    /// interrupted pc and then each frame's return address, walked by frame
    /// pointers. A function built without one leaves its caller out.
    FromUser = 1,
    /// Kernel records only: the unwind stopped at the depth cap, at a frame
    /// it could not read, or at assembly it cannot step through other than
    /// the entry code that began the stack; or it never started because the interrupted
    /// code was off its thread's stack, so the stack is a prefix of the real
    /// one. A user record is not marked; its walk ends at the first frame it
    /// cannot read.
    Truncated = 2,
};
USTL_ENABLE_ENUM_BITMASK(SamplerRecordFlags);

/// One sample.
struct SamplerRecord {
    /// SAMPLER_RECORD_MAGIC.
    u32 magic;
    /// How many of ips are filled, from 1.
    u8 depth;
    SamplerRecordFlags flags;
    /// The CPU whose buffer holds the record: for a kernel record the one
    /// the tick landed on, for a user record the one the thread walked its
    /// stack on, which is the same unless it moved in between.
    u16 cpu;
    /// The id of the thread it interrupted.
    u32 tid;
    u32 reserved;
    /// When the record was written, in monotonic nanoseconds: at the tick
    /// for a kernel record, after the walk for a user record. It only orders
    /// samples.
    u64 timestamp;
    /// The koid of the process the interrupted thread belongs to, which says
    /// whose binary a user record's ips are in; 0 for a kernel thread.
    /// Version 2.
    u64 pid;
    /// The interrupted pc, then the return address of each frame above it.
    u64 ips[SAMPLER_MAX_STACK_DEPTH];
};
static_assert(sizeof(SamplerRecord) == 160);
static_assert(offsetof(SamplerRecord, depth) == 4);
static_assert(offsetof(SamplerRecord, cpu) == 6);
static_assert(offsetof(SamplerRecord, tid) == 8);
static_assert(offsetof(SamplerRecord, timestamp) == 16);
static_assert(offsetof(SamplerRecord, pid) == 24);
static_assert(offsetof(SamplerRecord, ips) == 32);

/// The magic the dump header starts with: "OPRF" in memory order.
inline constexpr u32 SAMPLER_DUMP_MAGIC = 0x4652'504F;
/// 2 since each record says its process (SamplerRecord::pid) and the header
/// counts the ticks no target recorded (num_unmatched). Version 1 had
/// neither: 152-byte records with ips at 24, and a 32-byte header.
inline constexpr u16 SAMPLER_DUMP_VERSION = 2;

/// The dump's first bytes.
struct SamplerDumpHeader {
    /// SAMPLER_DUMP_MAGIC.
    u32 magic;
    /// SAMPLER_DUMP_VERSION.
    u16 version;
    /// sizeof(SamplerDumpHeader), and record_size sizeof(SamplerRecord): a
    /// reader can step over what it does not know.
    u16 header_size;
    u32 record_size;
    /// The records that follow.
    u32 num_records;
    /// Samples a full buffer had no room for, summed over the CPUs.
    u32 num_dropped;
    /// The CPUs the session had a buffer slot for.
    u32 num_cpus;
    /// Where the kernel image was loaded: subtract it from a kernel pc to
    /// get its address in the kernel's ELF.
    u64 load_base;
    /// Ticks that landed where no target was asked for -- kernel code in a
    /// user-only session, user code in a kernel-only one -- summed over the
    /// CPUs. With the records and the drops, the session's ticks, so a share
    /// of the time can be told from them. Version 2.
    u64 num_unmatched;
};
static_assert(sizeof(SamplerDumpHeader) == 40);
static_assert(alignof(SamplerDumpHeader) == 8);
} // namespace ours

#endif // #ifndef OURS_SAMPLER_HPP
