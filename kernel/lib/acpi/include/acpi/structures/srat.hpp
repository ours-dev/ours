/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_STRUCTURES_SRAT_HPP
#define ACPI_STRUCTURES_SRAT_HPP 1

#include <acpi/structures/header.hpp>

#include <ustl/util/enum_bits.hpp>

namespace acpi::srat {
/// Static resource affinity structure types.
///
/// Reference: ACPI v6.5 Table 5.61.
enum class Type : u8 {
    ProcessorAffinity = 0x00,
    MemoryAffinity = 0x01,
    ProcessorX2ApicAffinity = 0x02,
};

enum class Flag : u32 {
    Enabled = 0x01,
};
USTL_ENABLE_ENUM_BITMASK(Flag);

/// System resource affinity table.
///
/// Reference: ACPI v6.5 Section 5.2.16.
struct PACKED Table {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return header.length;
    }

    TableHeader header;
    u8 reserved[12];

    constexpr static Signature SIGNATURE = Signature::from_chars("SRAT");
};
static_assert(sizeof(Table) == 48);

/// Type 0x00: processor local APIC/SAPIC affinity structure.
///
/// Reference: ACPI v6.5 Section 5.2.16.1.
struct PACKED ProcessorAffinityEntry {
    /// The proximity domain is split across two non-adjacent fields for
    /// backwards compatibility with ACPI 1.0, where only the low byte existed.
    [[nodiscard]]
    constexpr auto proximity_domain() const -> u32 {
        return u32(proximity_domain_low) | (u32(proximity_domain_high[0]) << 8) |
               (u32(proximity_domain_high[1]) << 16) | (u32(proximity_domain_high[2]) << 24);
    }

    [[nodiscard]]
    constexpr auto enabled() const -> bool {
        return !!(flags & Flag::Enabled);
    }

    EntryHeader header;
    u8 proximity_domain_low;
    u8 apic_id;
    Flag flags;
    u8 sapic_eid;
    u8 proximity_domain_high[3];
    u32 clock_domain;

    constexpr static Type TYPE = Type::ProcessorAffinity;
};
static_assert(sizeof(ProcessorAffinityEntry) == 16);

/// Type 0x01: memory affinity structure.
///
/// Reference: ACPI v6.5 Section 5.2.16.2.
struct PACKED MemoryAffinityEntry {
    [[nodiscard]]
    constexpr auto base_address() const -> u64 {
        return u64(base_address_low) | (u64(base_address_high) << 32);
    }

    [[nodiscard]]
    constexpr auto length() const -> u64 {
        return u64(length_low) | (u64(length_high) << 32);
    }

    [[nodiscard]]
    constexpr auto enabled() const -> bool {
        return !!(flags & Flag::Enabled);
    }

    EntryHeader header;
    u32 proximity_domain;
    u16 reserved;
    u32 base_address_low;
    u32 base_address_high;
    u32 length_low;
    u32 length_high;
    u32 reserved2;
    Flag flags;
    u32 reserved3;
    u32 reserved4;

    constexpr static Type TYPE = Type::MemoryAffinity;
};
static_assert(sizeof(MemoryAffinityEntry) == 40);

/// Type 0x02: processor local x2APIC affinity structure.
///
/// Reference: ACPI v6.5 Section 5.2.16.3.
struct PACKED ProcessorX2ApicAffinityEntry {
    [[nodiscard]]
    constexpr auto enabled() const -> bool {
        return !!(flags & Flag::Enabled);
    }

    EntryHeader header;
    u16 reserved;
    u32 proximity_domain;
    u32 x2apic_id;
    Flag flags;
    u32 clock_domain;
    u32 reserved2;

    constexpr static Type TYPE = Type::ProcessorX2ApicAffinity;
};
static_assert(sizeof(ProcessorX2ApicAffinityEntry) == 24);

} // namespace acpi::srat

#endif // #ifndef ACPI_STRUCTURES_SRAT_HPP
