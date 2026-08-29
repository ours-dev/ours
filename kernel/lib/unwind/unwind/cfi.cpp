/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <unwind/cfi.hpp>

namespace unwind {
namespace {
    constexpr std::uint32_t DWARF64_ESCAPE = 0xFFFF'FFFF;
    constexpr std::uint64_t DEBUG_FRAME_CIE_ID32 = 0xFFFF'FFFF;
    constexpr std::size_t MAX_AUGMENTATION = 7;

    auto is_cie_id(Section::Kind kind, std::uint32_t id) -> bool {
        if (kind == Section::Kind::EhFrame) {
            return id == 0;
        }
        return id == DEBUG_FRAME_CIE_ID32;
    }
} // namespace

auto identify_entry(Section const &section, std::uint64_t offset, EntryKind *kind, std::uint64_t *length)
    -> bool {
    if (offset >= section.size) {
        return false;
    }

    Cursor cursor(section.data + offset, section.size - offset);
    std::uint32_t const record_length = cursor.u32();
    if (!cursor.ok()) {
        return false;
    }
    if (record_length == 0) {
        *kind = EntryKind::Terminator;
        *length = 4;
        return true;
    }
    if (record_length == DWARF64_ESCAPE) {
        // DWARF64 CFI is not produced by this toolchain.
        return false;
    }

    std::uint32_t const discriminator = cursor.u32();
    if (!cursor.ok()) {
        return false;
    }
    *kind = is_cie_id(section.kind, discriminator) ? EntryKind::Cie : EntryKind::Fde;
    *length = 4 + record_length;
    return true;
}

auto parse_cie(Section const &section, std::uint64_t offset, Cie *out) -> bool {
    if (offset >= section.size) {
        return false;
    }

    Cursor cursor(section.data + offset, section.size - offset);
    std::uint32_t const record_length = cursor.u32();
    std::uint32_t const discriminator = cursor.u32();
    std::uint8_t const version = cursor.u8();
    if (!cursor.ok()) {
        return false;
    }
    if (record_length == 0 || record_length == DWARF64_ESCAPE) {
        return false;
    }
    if (!is_cie_id(section.kind, discriminator)) {
        return false;
    }
    if (version == 0 || version > 4) {
        return false;
    }

    if (version >= 4) {
        std::uint8_t const address_size = cursor.u8();
        std::uint8_t const segment_size = cursor.u8();
        if (!cursor.ok()) {
            return false;
        }
        if (address_size != 8 || segment_size != 0) {
            return false;
        }
    }

    char augmentation[MAX_AUGMENTATION + 1] = {0};
    std::size_t augmentation_length = 0;
    while (true) {
        std::uint8_t const ch = cursor.u8();
        if (!cursor.ok()) {
            return false;
        }
        if (ch == 0) {
            break;
        }
        if (augmentation_length >= MAX_AUGMENTATION) {
            return false;
        }
        augmentation[augmentation_length] = static_cast<char>(ch);
        ++augmentation_length;
    }

    Cie cie = {};
    cie.offset = offset;
    cie.version = version;
    cie.code_align = cursor.uleb();
    cie.data_align = cursor.sleb();
    cie.ra_column = (version == 1) ? cursor.u8() : cursor.uleb();
    cie.fde_encoding = DW_EH_PE_ABSPTR;
    cie.augmentation_present = augmentation[0] == 'z';
    if (!cursor.ok() || cie.code_align == 0) {
        return false;
    }

    if (cie.augmentation_present) {
        std::uint64_t const augmentation_data_length = cursor.uleb();
        if (!cursor.ok()) {
            return false;
        }
        std::uint64_t const augmentation_data_end = cursor.offset() + augmentation_data_length;

        for (std::size_t i = 1; augmentation[i] != '\0'; ++i) {
            switch (augmentation[i]) {
                case 'L': {
                    cursor.u8();
                    break;
                }
                case 'P': {
                    std::uint8_t const personality_encoding = cursor.u8();
                    std::uint64_t const datum_base = offset + cursor.offset();
                    cursor.encoded_pointer(personality_encoding, datum_base);
                    break;
                }
                case 'R': {
                    cie.fde_encoding = cursor.u8();
                    break;
                }
                default:
                    return false;
            }
            if (!cursor.ok() || cursor.offset() > augmentation_data_end) {
                return false;
            }
        }
        cursor.skip(augmentation_data_end - cursor.offset());
        if (!cursor.ok()) {
            return false;
        }
    }

    if (cursor.offset() > record_length + 4) {
        return false;
    }
    cie.instructions_begin = offset + cursor.offset();
    cie.instructions_end = offset + 4 + record_length;
    *out = cie;
    return true;
}

auto parse_fde(Section const &section, std::uint64_t offset, Cie const &cie, Fde *out) -> bool {
    if (offset >= section.size) {
        return false;
    }

    Cursor cursor(section.data + offset, section.size - offset);
    std::uint32_t const record_length = cursor.u32();
    if (!cursor.ok() || record_length == 0 || record_length == DWARF64_ESCAPE) {
        return false;
    }

    std::uint32_t const cie_reference = cursor.u32();
    if (!cursor.ok()) {
        return false;
    }
    std::uint64_t cie_offset = 0;
    if (section.kind == Section::Kind::EhFrame) {
        // .eh_frame: the reference is relative to the field itself.
        cie_offset = offset + 4 - cie_reference;
    } else {
        // .debug_frame: the reference is a section offset.
        cie_offset = cie_reference;
    }
    if (cie_offset != cie.offset) {
        return false;
    }

    Fde fde = {};
    fde.offset = offset;
    fde.cie_offset = cie_offset;

    std::uint64_t const pc_base = offset + cursor.offset();
    fde.pc_begin = cursor.encoded_pointer(cie.fde_encoding, pc_base);
    if (!cursor.ok()) {
        return false;
    }

    // The range uses the CIE's encoding with the PC-relative application
    // stripped: a range is a length, not an address.
    std::uint8_t const range_encoding = cie.fde_encoding & DW_EH_PE_FORMAT_MASK;
    std::uint64_t const pc_range = cursor.encoded_pointer(range_encoding, 0);
    if (!cursor.ok()) {
        return false;
    }
    fde.pc_range = pc_range;

    if (cie.augmentation_present) {
        std::uint64_t const augmentation_data_length = cursor.uleb();
        if (!cursor.ok()) {
            return false;
        }
        cursor.skip(augmentation_data_length);
        if (!cursor.ok()) {
            return false;
        }
    }

    fde.instructions_begin = offset + cursor.offset();
    fde.instructions_end = offset + 4 + record_length;
    if (fde.instructions_begin > fde.instructions_end || fde.instructions_end > section.size) {
        return false;
    }
    *out = fde;
    return true;
}

} // namespace unwind
