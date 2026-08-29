/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_STRUCTURES_ROOT_HPP
#define ACPI_STRUCTURES_ROOT_HPP 1

#include <acpi/structures/header.hpp>

namespace acpi::sdt {
/// Root system description pointer, ACPI 1.0 layout.
///
/// Reference: ACPI v6.5 Section 5.2.5.3.
struct PACKED Rsdp {
    /// The signature is the eight bytes "RSD PTR ", carried here as two four
    /// byte halves because `Signature` is the natural unit everywhere else.
    ///
    /// Both halves must match.  This used to be spelled `||`, which let any
    /// eight bytes whose *first* half happened to read "RSD " pass validation
    /// and get dereferenced as an RSDP.
    [[nodiscard]]
    constexpr auto validate_signature() const -> bool {
        return sig_low == SIGNATURE_LOW && sig_high == SIGNATURE_HIGH;
    }

    /// Signature plus the ACPI 1.0 checksum, which covers the first 20 bytes.
    [[nodiscard]]
    auto validate() const -> bool;

    Signature sig_low;
    Signature sig_high;
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_addr; // Deprecated from revision 2 onwards; prefer RsdpV2::xsdt_addr.

    constexpr static Signature SIGNATURE_LOW = Signature::from_chars("RSD ");
    constexpr static Signature SIGNATURE_HIGH = Signature::from_chars("PTR ");

    /// `revision` is 0 for ACPI 1.0 and 2 for 2.0+; a revision below this has
    /// no XSDT and no extended checksum.
    constexpr static u8 REVISION_V2 = 2;
};
static_assert(sizeof(Rsdp) == 20);

/// Root system description pointer, ACPI 2.0+ layout.
///
/// Reference: ACPI v6.5 Section 5.2.5.3.
struct PACKED RsdpV2 {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return length;
    }

    /// The extended checksum, which covers all `length` bytes.  The ACPI 1.0
    /// checksum over the leading 20 bytes is a separate requirement and is
    /// verified through `v1.validate()`.
    [[nodiscard]]
    auto validate() const -> bool;

    Rsdp v1;
    u32 length;
    u64 xsdt_addr;
    u8 ext_checksum;
    u8 reserved[3];
};
static_assert(sizeof(RsdpV2) == 36);

/// Root system description table: an array of 32-bit table pointers.
///
/// Reference: ACPI v6.5 Section 5.2.7.
struct PACKED Rsdt {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return header.length;
    }

    /// Only meaningful once `header.length` has been validated to be at least
    /// `sizeof(Rsdt)`; TableSet does that before constructing.
    [[nodiscard]]
    constexpr auto count() const -> usize {
        return (header.length - sizeof(Rsdt)) / sizeof(u32);
    }

    TableHeader header;
    u32 addr32[];

    constexpr static Signature SIGNATURE = Signature::from_chars("RSDT");
};
static_assert(sizeof(Rsdt) == 36);

/// Extended system description table: an array of 64-bit table pointers.
///
/// Reference: ACPI v6.5 Section 5.2.8.
struct PACKED Xsdt {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return header.length;
    }

    [[nodiscard]]
    constexpr auto count() const -> usize {
        return (header.length - sizeof(Xsdt)) / sizeof(u64);
    }

    TableHeader header;
    u64 addr64[];

    constexpr static Signature SIGNATURE = Signature::from_chars("XSDT");
};
static_assert(sizeof(Xsdt) == 36);

} // namespace acpi::sdt

#endif // #ifndef ACPI_STRUCTURES_ROOT_HPP
