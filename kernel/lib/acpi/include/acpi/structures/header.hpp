/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_STRUCTURES_HEADER_HPP
#define ACPI_STRUCTURES_HEADER_HPP 1

#include <acpi/signature.hpp>
#include <acpi/types.hpp>

namespace acpi {
/// Header shared by the variable length sub-entries that follow a table body
/// (MADT interrupt controller structures, SRAT affinity structures, ...).
///
/// Reference: ACPI v6.5 Section 5.2.12.
struct PACKED EntryHeader {
    [[nodiscard]]
    constexpr auto size() const -> u8 {
        return length;
    }

    u8 type;
    u8 length;
};
static_assert(sizeof(EntryHeader) == 2);

/// System description table header, common to every ACPI table.
///
/// Reference: ACPI v6.5 Section 5.2.6.
struct PACKED TableHeader {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return length;
    }

    Signature sign;
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
};
static_assert(sizeof(TableHeader) == 36);

/// Generic address structure.
///
/// Reference: ACPI v6.5 Section 5.2.3.2.
struct PACKED GenericAddress {
    u8 address_space_id;
    u8 register_bit_width;
    u8 register_bit_offset;
    u8 access_size;
    u64 address;
};
static_assert(sizeof(GenericAddress) == 12);

enum class AddressSpace : u8 {
    SystemMemory = 0,
    SystemIo = 1,
};

} // namespace acpi

#endif // #ifndef ACPI_STRUCTURES_HEADER_HPP
