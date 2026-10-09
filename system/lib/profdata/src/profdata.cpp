/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// PROVENANCE: adapted from Fuchsia's src/lib/llvm-profdata (Copyright 2021 The
/// Fuchsia Authors, BSD-style license).
#include <profdata/profdata.hpp>

#include <algorithm>
#include <array>
#include <atomic>

#include <cstdint>
#include <cstring>

#include <ours/assert.hpp>

// The compiler's own description of the raw profile format: the constants first,
// and below, the same file again once per table it expands.
#include <profile/InstrProfData.inc>

// NOLINTBEGIN(readability-identifier-naming): everything in this block is a
// symbol the compiler's instrumentation refers to by name, so it cannot follow
// this repository's case rules.
extern "C" {

// The format version the header records. The compiler sometimes emits this with
// a different value, and the header has to say whichever value the program was
// linked with; this is the default for when it does not.
[[gnu::weak]]
extern u64 const INSTR_PROF_RAW_VERSION_VAR = INSTR_PROF_RAW_VERSION;

} // extern "C"
// NOLINTEND(readability-identifier-naming)

namespace profdata {
namespace {

    constexpr u64 MAGIC = INSTR_PROF_RAW_MAGIC_64;

    // NOLINTBEGIN(readability-identifier-naming): as above, these are the names
    // the tables in InstrProfData.inc expand to.

    using IntPtrT = std::intptr_t;

    enum ValueKind {
#define VALUE_PROF_KIND(Enumerator, Value, Descr) Enumerator = Value,
#include <profile/InstrProfData.inc>
    };

    // One record per instrumented function: where its counters are, and how
    // many. Present in the profile only when the compiler was not asked to keep
    // them in the binary.
    struct ProfileData {
#define INSTR_PROF_DATA(Type, LLVMType, Name, Initializer) Type Name;
#include <profile/InstrProfData.inc>
    };

    struct alignas(INSTR_PROF_DATA_ALIGNMENT) VTableProfData {
#define INSTR_PROF_VTABLE_DATA(Type, LLVMType, Name, Initializer) Type Name;
#include <profile/InstrProfData.inc>
    };

    // The compiler puts each kind of instrumentation data in a section of its
    // own, and the linker names where each one begins and ends. The names
    // differ by object format; the pairs below are what the rest of this file
    // reads.
#if defined(__APPLE__)

    extern "C" {

    [[gnu::visibility("hidden")]]
    extern ProfileData const DataBegin[] __asm__("section$start$__DATA$" INSTR_PROF_DATA_SECT_NAME);
    [[gnu::visibility("hidden")]]
    extern ProfileData const DataEnd[] __asm__("section$end$__DATA$" INSTR_PROF_DATA_SECT_NAME);

    [[gnu::visibility("hidden")]]
    extern char const NamesBegin[] __asm__("section$start$__DATA$" INSTR_PROF_NAME_SECT_NAME);
    [[gnu::visibility("hidden")]]
    extern char const NamesEnd[] __asm__("section$end$__DATA$" INSTR_PROF_NAME_SECT_NAME);

    [[gnu::visibility("hidden")]]
    extern VTableProfData const
        VTableDataBegin[] __asm__("section$start$__DATA$" INSTR_PROF_VTAB_SECT_NAME);
    [[gnu::visibility("hidden")]]
    extern VTableProfData const
        VTableDataEnd[] __asm__("section$end$__DATA$" INSTR_PROF_VTAB_SECT_NAME);

    [[gnu::visibility("hidden")]]
    extern char const VNamesBegin[] __asm__("section$start$__DATA$" INSTR_PROF_VNAME_SECT_NAME);
    [[gnu::visibility("hidden")]]
    extern char const VNamesEnd[] __asm__("section$end$__DATA$" INSTR_PROF_VNAME_SECT_NAME);

    [[gnu::visibility("hidden")]]
    extern char CountersBegin[] __asm__("section$start$__DATA$" INSTR_PROF_CNTS_SECT_NAME);
    [[gnu::visibility("hidden")]]
    extern char CountersEnd[] __asm__("section$end$__DATA$" INSTR_PROF_CNTS_SECT_NAME);

    [[gnu::visibility("hidden")]]
    extern char BitmapBegin[] __asm__("section$start$__DATA$" INSTR_PROF_BITS_SECT_NAME);
    [[gnu::visibility("hidden")]]
    extern char BitmapEnd[] __asm__("section$end$__DATA$" INSTR_PROF_BITS_SECT_NAME);

    [[gnu::visibility("hidden")]]
    extern char UniformCountersBegin[] __asm__(
        "section$start$__DATA$" INSTR_PROF_QUOTE(INSTR_PROF_UCNTS_COMMON));
    [[gnu::visibility("hidden")]]
    extern char UniformCountersEnd[] __asm__(
        "section$end$__DATA$" INSTR_PROF_QUOTE(INSTR_PROF_UCNTS_COMMON));

    } // extern "C"

#elif defined(__ELF__)

    // An ELF linker defines __start_SECNAME and __stop_SECNAME for every output
    // section it makes, and makes none when no input has the section. A program
    // that was not instrumented, or whose instrumentation sections were all
    // garbage-collected, would then fail to link. So each section is also
    // declared here, empty and retained, which makes the linker produce the
    // section and the two symbols and costs the image nothing: the symbols are
    // equal and the loops across them do nothing.
    //
    // C++ has no way to put a section name into an assembler label except
    // through the preprocessor, hence the macro.
    extern "C" {

#define PROFDATA_SECTION(type, begin, end, section, writable)                                      \
    [[gnu::visibility("hidden")]]                                                                  \
    extern type begin[] __asm__(INSTR_PROF_QUOTE(INSTR_PROF_SECT_START(section)));                 \
    [[gnu::visibility("hidden")]]                                                                  \
    extern type end[] __asm__(INSTR_PROF_QUOTE(INSTR_PROF_SECT_STOP(section)));                    \
    __asm__(".pushsection " INSTR_PROF_QUOTE(section) ",\"aR" writable "\",%progbits\n"            \
                                                      ".popsection")

    PROFDATA_SECTION(ProfileData const, DataBegin, DataEnd, INSTR_PROF_DATA_COMMON, "");
    PROFDATA_SECTION(char const, NamesBegin, NamesEnd, INSTR_PROF_NAME_COMMON, "");
    PROFDATA_SECTION(VTableProfData const, VTableDataBegin, VTableDataEnd, INSTR_PROF_VTAB_COMMON,
                     "");
    PROFDATA_SECTION(char const, VNamesBegin, VNamesEnd, INSTR_PROF_VNAME_COMMON, "");
    PROFDATA_SECTION(char, CountersBegin, CountersEnd, INSTR_PROF_CNTS_COMMON, "w");
    PROFDATA_SECTION(char, BitmapBegin, BitmapEnd, INSTR_PROF_BITS_COMMON, "w");
    PROFDATA_SECTION(char, UniformCountersBegin, UniformCountersEnd, INSTR_PROF_UCNTS_COMMON, "w");

    } // extern "C"

#else
#error "profdata: this object file format is not supported"
#endif

    // The raw profile's header, field for field as InstrProfData.inc lists them.
    struct RawHeader {
        [[nodiscard]]
        auto binary_ids_size() const -> usize {
            return static_cast<usize>(BinaryIdsSize);
        }

#define INSTR_PROF_RAW_HEADER(Type, Name, Initializer) Type Name;
#include <profile/InstrProfData.inc>
    };

    // NOLINTEND(readability-identifier-naming)

    // A profile is made of chunks, and each chunk after the build ID starts on
    // an 8-byte boundary.
    constexpr usize ALIGN_AFTER_BUILD_ID = sizeof(u64);

    constexpr auto padding_size(usize const chunk_size_bytes) -> usize {
        return (ALIGN_AFTER_BUILD_ID - (chunk_size_bytes % ALIGN_AFTER_BUILD_ID)) %
               ALIGN_AFTER_BUILD_ID;
    }

    constexpr auto padding_size(std::span<std::byte const> const chunk) -> usize {
        return padding_size(chunk.size_bytes());
    }

    // The binary IDs chunk: the ID's length, the ID, and padding. Empty when
    // there is no ID.
    constexpr auto binary_ids_size(std::span<std::byte const> const build_id) -> usize {
        if (build_id.empty()) {
            return 0;
        }
        return sizeof(u64) + build_id.size_bytes() + padding_size(build_id);
    }

    template <typename T>
    auto array_between(T *const begin, T *const end) -> std::span<T> {
        auto const begin_bytes = reinterpret_cast<std::byte const *>(begin);
        auto const end_bytes = reinterpret_cast<std::byte const *>(end);
        auto const size_bytes = static_cast<usize>(end_bytes - begin_bytes);
        return {begin, (size_bytes + sizeof(T) - 1) / sizeof(T)};
    }

    auto function_records() -> std::span<ProfileData const> {
        return array_between(DataBegin, DataEnd);
    }

    auto vtable_records() -> std::span<VTableProfData const> {
        return array_between(VTableDataBegin, VTableDataEnd);
    }

    // The counters, which instrumented code updates in place as it runs.
    auto counters() -> std::span<char> {
        return {&CountersBegin[0], static_cast<usize>(CountersEnd - CountersBegin)};
    }

    auto bitmap() -> std::span<char> {
        return {&BitmapBegin[0], static_cast<usize>(BitmapEnd - BitmapBegin)};
    }

    auto uniform_counters() -> std::span<char> {
        return {&UniformCountersBegin[0],
                static_cast<usize>(UniformCountersEnd - UniformCountersBegin)};
    }

    // Whether the compiler counted in single bytes, which it can be asked to do
    // when all that matters is whether a region ran.
    auto using_single_byte_counters() -> bool {
        return (INSTR_PROF_RAW_VERSION_VAR & VARIANT_MASK_BYTE_COVERAGE) != 0;
    }

    auto counter_size() -> usize {
        if (using_single_byte_counters()) {
            return sizeof(u8);
        }
        return sizeof(u64);
    }

    auto make_header(std::span<std::byte const> const build_id) -> RawHeader {
        // NOLINTBEGIN(readability-identifier-naming): the INSTR_PROF_RAW_HEADER
        // initializers in InstrProfData.inc read these by name.
        u64 const NumData = function_records().size();
        u64 const PaddingBytesBeforeCounters = 0;
        u64 const NumCounters = counters().size() / counter_size();
        u64 const PaddingBytesAfterCounters = padding_size(counters().size());
        u64 const NumBitmapBytes = bitmap().size();
        u64 const PaddingBytesAfterBitmapBytes = 0;
        u64 const NamesSize = static_cast<u64>(NamesEnd - NamesBegin);
        u64 const NumVTables = vtable_records().size();
        u64 const VNamesSize = static_cast<u64>(VNamesEnd - VNamesBegin);
        u64 const NumUniformCounters = uniform_counters().size() / sizeof(u64);
        u64 const PaddingBytesAfterUniformCounters = padding_size(uniform_counters().size());
        auto const __llvm_profile_get_magic = []() -> u64 { return MAGIC; };
        auto const __llvm_profile_get_version = []() -> u64 { return INSTR_PROF_RAW_VERSION_VAR; };
        auto const __llvm_write_binary_ids = [build_id](void *const ignored) -> u64 {
            DEBUG_ASSERT(ignored == nullptr);
            return binary_ids_size(build_id);
        };

        RawHeader header = {
#define INSTR_PROF_RAW_HEADER(Type, Name, Initializer) .Name = Initializer,
#include <profile/InstrProfData.inc>
        };
        // NOLINTEND(readability-identifier-naming)

        // With binary correlation the function records and names are left out
        // of the profile, and the deltas that would have located them mean
        // nothing.
        if (NumData == 0 && NamesSize == 0) {
            header.CountersDelta = 0;
            header.NamesDelta = 0;
            header.BitmapDelta = 0;
            header.UniformCountersDelta = 0;
        }

        return header;
    }

    // Whether no function was instrumented, in which case there is nothing to
    // write.
    auto has_no_data() -> bool {
        return counters().empty() && bitmap().empty() && uniform_counters().empty();
    }

    // Copies `from`, which instrumented code may be incrementing, into `to`, one
    // T at a time with a relaxed atomic load of each. The compiler's own stores
    // are plain stores, which this accepts: on the architectures this builds for
    // an aligned store of this size is one instruction and cannot be seen half
    // done.
    template <typename T>
    auto copy_relaxed(std::span<std::byte> const to, std::span<char> const from, char const *what)
        -> void {
        // A kind of data the program has none of is an empty section, whose
        // start the linker may put anywhere, aligned or not.
        if (from.empty()) {
            return;
        }
        ASSERT(to.size_bytes() >= from.size_bytes(), "copying {} bytes of {} into only {} bytes",
               from.size_bytes(), what, to.size_bytes());
        ASSERT(from.size_bytes() % sizeof(T) == 0 &&
                   reinterpret_cast<std::uintptr_t>(from.data()) % alignof(T) == 0,
               "{} are not an aligned array of {}-byte counters", what, sizeof(T));

        auto *const source = reinterpret_cast<T *>(from.data());
        auto const count = from.size_bytes() / sizeof(T);
        for (usize i = 0; i < count; ++i) {
            T const value = std::atomic_ref<T>(source[i]).load(std::memory_order_relaxed);
            std::memcpy(to.data() + (i * sizeof(T)), &value, sizeof(T));
        }
    }

} // namespace

RawProfile::RawProfile(std::span<std::byte const> const build_id) : m_build_id{build_id} {
    if (has_no_data()) {
        return;
    }

    // The order and sizes here have to match write_fixed().
    auto const header = make_header(m_build_id);

    m_counters_offset = sizeof(header) + header.binary_ids_size() +
                        (static_cast<usize>(header.NumData) * sizeof(ProfileData)) +
                        static_cast<usize>(header.PaddingBytesBeforeCounters);
    m_counters_size_bytes = static_cast<usize>(header.NumCounters) * counter_size();
    ASSERT(m_counters_size_bytes == counters().size_bytes(), "the header counts {} counter bytes",
           m_counters_size_bytes);

    auto next_offset = m_counters_offset + m_counters_size_bytes +
                       static_cast<usize>(header.PaddingBytesAfterCounters);

    m_bitmap_offset = next_offset;
    m_bitmap_size_bytes = static_cast<usize>(header.NumBitmapBytes);
    ASSERT(m_bitmap_size_bytes == bitmap().size_bytes(), "the header counts {} bitmap bytes",
           m_bitmap_size_bytes);
    next_offset = m_bitmap_offset + m_bitmap_size_bytes +
                  static_cast<usize>(header.PaddingBytesAfterBitmapBytes);

    m_uniform_counters_offset = next_offset;
    m_uniform_counters_size_bytes = static_cast<usize>(header.NumUniformCounters) * sizeof(u64);
    ASSERT(m_uniform_counters_size_bytes == uniform_counters().size_bytes(),
           "the header counts {} uniform counter bytes", m_uniform_counters_size_bytes);
    next_offset = m_uniform_counters_offset + m_uniform_counters_size_bytes +
                  static_cast<usize>(header.PaddingBytesAfterUniformCounters);

    m_size_bytes = next_offset;

    auto const names_size = static_cast<usize>(header.NamesSize);
    m_size_bytes += names_size + padding_size(names_size);

    auto const vtables_size = static_cast<usize>(header.NumVTables) * sizeof(VTableProfData);
    m_size_bytes += vtables_size + padding_size(vtables_size);

    auto const vnames_size = static_cast<usize>(header.VNamesSize);
    m_size_bytes += vnames_size + padding_size(vnames_size);
}

auto RawProfile::size_bytes() const -> usize {
    return m_size_bytes;
}

auto RawProfile::write_fixed(std::span<std::byte> dump) const -> LiveRegions {
    if (m_size_bytes == 0) {
        return {};
    }
    ASSERT(dump.size_bytes() >= m_size_bytes, "a dump needs {} bytes and has {}", m_size_bytes,
           dump.size_bytes());
    ASSERT(reinterpret_cast<std::uintptr_t>(dump.data()) % ALIGNMENT == 0,
           "a dump must start on an {}-byte boundary", ALIGNMENT);

    // Writes `bytes` at the start of `dump` and moves `dump` past them.
    auto const write_bytes = [&dump](std::span<std::byte const> const bytes, char const *what) {
        ASSERT(dump.size_bytes() >= bytes.size_bytes(), "{} of {} bytes with only {} bytes left",
               what, bytes.size_bytes(), dump.size_bytes());
        std::memcpy(dump.data(), bytes.data(), bytes.size());
        dump = dump.subspan(bytes.size());
    };

    // Moves `dump` past `size` bytes that are not written now, and returns them.
    auto const reserve = [&dump](usize const size, char const *what) {
        ASSERT(dump.size_bytes() >= size, "{} bytes of {} with only {} bytes left", size, what,
               dump.size_bytes());
        auto const reserved = dump.subspan(0, size);
        dump = dump.subspan(size);
        return reserved;
    };

    constexpr std::array<std::byte, sizeof(u64)> PADDING_BYTES{};
    std::span<std::byte const> const padding{PADDING_BYTES};
    constexpr char const *PADDING = "alignment padding";

    // The chunks, in the order the format puts them. The order and sizes must
    // match the constructor's.
    auto const header = make_header(m_build_id);
    write_bytes(std::as_bytes(std::span{&header, 1}), "the raw profile header");

    u64 const build_id_size = m_build_id.size_bytes();
    if (build_id_size > 0) {
        write_bytes(std::as_bytes(std::span{&build_id_size, 1}), "the build ID size");
        write_bytes(std::as_bytes(m_build_id), "the build ID");
        write_bytes(padding.subspan(0, padding_size(m_build_id)), PADDING);
    }

    write_bytes(std::as_bytes(function_records()), INSTR_PROF_DATA_SECT_NAME);
    write_bytes(padding.subspan(0, static_cast<usize>(header.PaddingBytesBeforeCounters)), PADDING);

    LiveRegions regions;
    regions.counters = reserve(m_counters_size_bytes, "counters");
    write_bytes(padding.subspan(0, static_cast<usize>(header.PaddingBytesAfterCounters)), PADDING);

    regions.bitmap = reserve(m_bitmap_size_bytes, "bitmap");
    write_bytes(padding.subspan(0, static_cast<usize>(header.PaddingBytesAfterBitmapBytes)),
                PADDING);

    regions.uniform_counters = reserve(m_uniform_counters_size_bytes, "uniform counters");
    write_bytes(padding.subspan(0, static_cast<usize>(header.PaddingBytesAfterUniformCounters)),
                PADDING);

    std::span const names{NamesBegin, static_cast<usize>(NamesEnd - NamesBegin)};
    write_bytes(std::as_bytes(names), INSTR_PROF_NAME_SECT_NAME);
    write_bytes(padding.subspan(0, padding_size(static_cast<usize>(header.NamesSize))), PADDING);

    auto const vtables = vtable_records();
    write_bytes(std::as_bytes(vtables), INSTR_PROF_VTAB_SECT_NAME);
    write_bytes(padding.subspan(0, padding_size(vtables.size_bytes())), PADDING);

    std::span const vnames{VNamesBegin, static_cast<usize>(VNamesEnd - VNamesBegin)};
    write_bytes(std::as_bytes(vnames), INSTR_PROF_VNAME_SECT_NAME);
    write_bytes(padding.subspan(0, padding_size(vnames.size_bytes())), PADDING);

    return regions;
}

auto RawProfile::copy_live(LiveRegions const &regions) const -> void {
    if (using_single_byte_counters()) {
        copy_relaxed<u8>(regions.counters, counters(), "counters");
    } else {
        copy_relaxed<u64>(regions.counters, counters(), "counters");
    }
    copy_relaxed<u8>(regions.bitmap, bitmap(), "bitmap");
    copy_relaxed<u64>(regions.uniform_counters, uniform_counters(), "uniform counters");
}

} // namespace profdata
