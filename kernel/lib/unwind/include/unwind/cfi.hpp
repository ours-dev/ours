/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef UNWIND_CFI_HPP
#define UNWIND_CFI_HPP 1

#include <unwind/cursor.hpp>

#include <cstddef>
#include <cstdint>

namespace unwind {

// DW_EH_PE pointer-encoding classes (DW_EH_PE_*), lower nibble is the value
// format, the second nibble the application, 0x80 an indirection flag.
//
// The format nibble is not a dense enumeration: bits 0-2 pick the width
// (uleb128, 2, 4, 8 bytes) and bit 3 (0x08) marks the signed variant, so the
// signed formats sit at 0x09-0x0C, one bit away from their unsigned
// counterparts (LSB 4.1 "DWARF Extensions", GCC `dwarf2.h`). Hence the
// x86-64 psABI's usual CIE "R" byte, 0x1B, decomposes as pcrel | sdata4.
constexpr std::uint8_t DW_EH_PE_ABSPTR = 0x00;
constexpr std::uint8_t DW_EH_PE_ULEB128 = 0x01;
constexpr std::uint8_t DW_EH_PE_UDATA2 = 0x02;
constexpr std::uint8_t DW_EH_PE_UDATA4 = 0x03;
constexpr std::uint8_t DW_EH_PE_UDATA8 = 0x04;
constexpr std::uint8_t DW_EH_PE_SIGNED = 0x08;
constexpr std::uint8_t DW_EH_PE_SLEB128 = DW_EH_PE_ULEB128 | DW_EH_PE_SIGNED;
constexpr std::uint8_t DW_EH_PE_SDATA2 = DW_EH_PE_UDATA2 | DW_EH_PE_SIGNED;
constexpr std::uint8_t DW_EH_PE_SDATA4 = DW_EH_PE_UDATA4 | DW_EH_PE_SIGNED;
constexpr std::uint8_t DW_EH_PE_SDATA8 = DW_EH_PE_UDATA8 | DW_EH_PE_SIGNED;

constexpr std::uint8_t DW_EH_PE_PCREL = 0x10;
constexpr std::uint8_t DW_EH_PE_TEXTREL = 0x20;
constexpr std::uint8_t DW_EH_PE_DATAREL = 0x30;
constexpr std::uint8_t DW_EH_PE_FUNCREL = 0x40;
constexpr std::uint8_t DW_EH_PE_ALIGNED = 0x50;
constexpr std::uint8_t DW_EH_PE_INDIRECT = 0x80;
constexpr std::uint8_t DW_EH_PE_OMIT = 0xFF;

constexpr std::uint8_t DW_EH_PE_FORMAT_MASK = 0x0F;
constexpr std::uint8_t DW_EH_PE_APPLICATION_MASK = 0x70;

/// A CFI section as loaded in memory. `.eh_frame` uses PC-relative pointer
/// encodings and is safe to consume wherever the image is placed;
/// `.debug_frame` uses absolute encodings and is only parseable when its
/// addresses match the runtime layout.
struct Section final {
    std::uint8_t const *data;
    std::size_t size;
    enum class Kind : std::uint8_t {
        EhFrame,
        DebugFrame,
    };
    Kind kind;
};

/// Which entry a scan is looking at. In `.eh_frame` the 32-bit CIE
/// discriminator is 0; in `.debug_frame` it is all-ones.
enum class EntryKind : std::uint8_t {
    Cie,
    Fde,
    Terminator,
};

/// The subset of a CIE this unwinder consumes. `instructions_begin`/`_end`
/// are section-relative offsets of the CIE's initial-instruction program.
struct Cie final {
    std::uint64_t offset;
    std::uint64_t instructions_begin;
    std::uint64_t instructions_end;
    std::uint8_t version;
    std::uint64_t code_align;
    std::int64_t data_align;
    std::uint64_t ra_column;
    std::uint8_t fde_encoding;
    bool augmentation_present;
};

/// The subset of an FDE this unwinder consumes. Instruction bounds are
/// section-relative offsets, resolved lazily so the index build stays cheap.
struct Fde final {
    std::uint64_t offset;
    std::uint64_t cie_offset;
    std::uint64_t pc_begin;
    std::uint64_t pc_range;
    std::uint64_t instructions_begin;
    std::uint64_t instructions_end;
};

/// Classify and size one CFI record starting at `offset`. On success `*length`
/// holds the total record size in bytes (including the length field itself).
auto identify_entry(Section const &section, std::uint64_t offset, EntryKind *kind,
                    std::uint64_t *length) -> bool;

/// Parse the CIE at `offset`. Returns false on malformed input or on
/// augmentation features this unwinder cannot evaluate.
auto parse_cie(Section const &section, std::uint64_t offset, Cie *out) -> bool;

/// Parse the FDE at `offset`. `cie` must be the FDE's own CIE, since the
/// pointer encoding of `pc_begin`/`pc_range` lives in the CIE's `R`
/// augmentation. The PC-relative anchor is derived from the field position.
auto parse_fde(Section const &section, std::uint64_t offset, Cie const &cie, Fde *out) -> bool;

} // namespace unwind

#endif // #ifndef UNWIND_CFI_HPP
