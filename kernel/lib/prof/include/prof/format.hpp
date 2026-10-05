/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_FORMAT_HPP
#define PROF_FORMAT_HPP 1

#include <ustl/util/enum_bits.hpp>

namespace prof {

/// Fixed-size sample record shared verbatim between the kernel export path
/// and the host `prof-fold` tool. Addresses stay un-symbolized: the host
/// resolves them against the kernel ELF.
inline constexpr u32 SAMPLE_MAGIC = 0x504D'4153; // "SAMP"
inline constexpr usize MAX_STACK_DEPTH = 16;

enum class RecordFlags : u8 {
    RecordNone = 0,
    /// The sample interrupted user code; only `ips[0]` (the user PC) is
    /// meaningful and no kernel unwind was attempted.
    RecordFromUspace = 1,
    /// The unwind hit its depth cap or aborted early; the stack is a prefix.
    RecordTruncated = 2,
};

USTL_ENABLE_ENUM_BITMASK(RecordFlags);

struct SampleRecord final {
    u32 magic;
    u8 depth;
    RecordFlags flags;
    u16 cpu;
    u32 tid;
    u32 reserved;
    /// When the sample was taken, in monotonic nanoseconds
    /// (`SampleContext::timestamp`). The name is the wire format's: the field
    /// held the TSC until the kernel started supplying the time itself, and
    /// either way it only orders samples.
    u64 tsc;
    u64 ips[MAX_STACK_DEPTH];
};
static_assert(sizeof(SampleRecord) == 4 + 1 + 1 + 2 + 4 + 4 + 8 + 8 * MAX_STACK_DEPTH);
static_assert(sizeof(SampleRecord) == 152);

/// Dump container header, written once per `prof.dump` before the records.
inline constexpr u32 DUMP_MAGIC = 0x4652'504F; // "OPRF"
inline constexpr u16 DUMP_VERSION = 1;

struct DumpHeader final {
    u32 magic;
    u16 version;
    u16 header_size;
    u32 record_size;
    u32 num_records;
    u32 num_dropped;
    u32 num_cpus;
    /// Runtime address the kernel image was loaded at (the CLI subtracts it
    /// from record IPs before resolving ELF symbols).
    u64 load_base;
};
static_assert(sizeof(DumpHeader) == 32);
static_assert(alignof(DumpHeader) == 8);

/// Serial export framing: the header goes out as one `PROF1 <hex>` line and
/// every record as one `SAMP <hex>` line. Line framing survives the texty
/// serial console (and QEMU `-serial file:`) without binary escapes; the
/// magic doubles as a resync point after line corruption.
inline constexpr char DUMP_HEADER_TAG[6] = "PROF1";
inline constexpr char DUMP_RECORD_TAG[5] = "SAMP";

} // namespace prof

#endif // #ifndef PROF_FORMAT_HPP
