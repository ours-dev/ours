/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_FORMAT_HPP
#define PROF_FORMAT_HPP 1

#include <cstddef>
#include <cstdint>

namespace prof {

/// Fixed-size sample record shared verbatim between the kernel export path
/// and the host `prof-fold` tool. Addresses stay un-symbolized: the host
/// resolves them against the kernel ELF.
inline constexpr std::uint32_t SAMPLE_MAGIC = 0x504D'4153; // "SAMP"
inline constexpr std::size_t MAX_STACK_DEPTH = 16;

enum RecordFlags : std::uint8_t {
    RECORD_NONE = 0,
    /// The sample interrupted user code; only `ips[0]` (the user PC) is
    /// meaningful and no kernel unwind was attempted.
    RECORD_FROM_USPACE = 1,
    /// The unwind hit its depth cap or aborted early; the stack is a prefix.
    RECORD_TRUNCATED = 2,
};

struct SampleRecord final {
    std::uint32_t magic;
    std::uint8_t depth;
    std::uint8_t flags;
    std::uint16_t cpu;
    std::uint32_t tid;
    std::uint32_t reserved;
    std::uint64_t tsc;
    std::uint64_t ips[MAX_STACK_DEPTH];
};
static_assert(sizeof(SampleRecord) == 4 + 1 + 1 + 2 + 4 + 4 + 8 + 8 * MAX_STACK_DEPTH);
static_assert(sizeof(SampleRecord) == 152);

/// Dump container header, written once per `prof.dump` before the records.
inline constexpr std::uint32_t DUMP_MAGIC = 0x4652'504F; // "OPRF"
inline constexpr std::uint16_t DUMP_VERSION = 1;

struct DumpHeader final {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t header_size;
    std::uint32_t record_size;
    std::uint32_t num_records;
    std::uint32_t num_dropped;
    std::uint64_t tsc_hz;
    std::uint32_t num_cpus;
    std::uint32_t reserved;
};
static_assert(sizeof(DumpHeader) == 32);

/// Serial export framing: the header goes out as one `PROF1 <hex>` line and
/// every record as one `SAMP <hex>` line. Line framing survives the texty
/// serial console (and QEMU `-serial file:`) without binary escapes; the
/// magic doubles as a resync point after line corruption.
inline constexpr char DUMP_HEADER_TAG[6] = "PROF1";
inline constexpr char DUMP_RECORD_TAG[5] = "SAMP";

} // namespace prof

#endif // #ifndef PROF_FORMAT_HPP
