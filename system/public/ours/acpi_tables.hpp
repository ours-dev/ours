/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ACPI_TABLES_HPP
#define OURS_ACPI_TABLES_HPP 1

#include <ours/types.hpp>

#include <cstddef>

// The ACPI tables the firmware gave the machine, as the kernel hands them to
// user space: one read-only VMO, the startup handle HandleType::VmoAcpi, which
// the root serves on as the built-in protocol ours.boot.Acpi. The layout is ABI
// and never changes; a different one gets a new version.
//
//   [AcpiDirectory][AcpiDirectoryEntry x count][padding][table][padding][table]...
//
// Every field is little-endian. A table is the firmware's own bytes, exactly
// as it was, so a physical address inside one (the FADT's pointer to the DSDT,
// say) still holds the firmware's address and means nothing here: a reader
// finds a table through the directory. The RSDP and the root table are not in
// the VMO, since the directory is their replacement, and neither is the FACS,
// which holds no AML.
//
// The DSDT comes first when the machine has one, which the root table does not
// list: the FADT points to it. The tables the root table lists follow, in its
// order, so several SSDTs keep the order the firmware gave them. A table whose
// checksum does not hold is not in the VMO.
namespace ours {
/// "ACPI" as ACPI spells a signature: the first character in the low byte.
inline constexpr auto ACPI_DIRECTORY_MAGIC = u32{0x4950'4341};

inline constexpr auto ACPI_DIRECTORY_VERSION = u32{1};

/// Every table starts at an offset that is a multiple of this, so that a
/// reader may use a table's fields in place.
inline constexpr auto ACPI_TABLE_ALIGNMENT = usize{8};

struct AcpiDirectory {
    u32 magic;   ///< ACPI_DIRECTORY_MAGIC
    u32 version; ///< ACPI_DIRECTORY_VERSION
    u32 count;   ///< The number of entries after this header
    u32 size; ///< The bytes the directory and the tables take. The VMO is that, rounded up to a page
};
static_assert(sizeof(AcpiDirectory) == 16);

struct AcpiDirectoryEntry {
    u32 signature; ///< The table's four characters, the first in the low byte
    u32 offset;    ///< From the start of the VMO
    u32 length;    ///< The table's own length, from its header
    u32 reserved;  ///< Zero
};
static_assert(sizeof(AcpiDirectoryEntry) == 16);
static_assert(offsetof(AcpiDirectoryEntry, offset) == 4);

} // namespace ours

#endif // #ifndef OURS_ACPI_TABLES_HPP
