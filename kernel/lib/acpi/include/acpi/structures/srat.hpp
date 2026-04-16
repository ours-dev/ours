/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_SRAT_HPP
#define ACPI_SRAT_HPP 1

#include <acpi/structures/header.hpp>

#include <ustl/util/enum_bits.hpp>

namespace acpi::srat {
    enum class Type: u8 {
        ProcessorAffinity,
        MemoryAffinity,
        ProcessorX2ApicAffinity,
        AcpiSratFlagEnabled = 1,
    };

    enum class Flag: u32 {
        Enabled = 0x01,
    };
    USTL_ENABLE_ENUM_BITMASK(Flag);

    // SRAT table and descriptors.
    //
    // Reference: ACPI v6.3 Section 5.2.16.
    struct PACKED Table {
        TableHeader header;
        u8 reserved[12];

        auto size() const -> usize {
            return header.length;
        }
        static constexpr auto kSignature = Signature("SRAT");
    };
    static_assert(sizeof(Table) == 48);

    /// Type 0: processor local apic/sapic affinity structure
    ///
    /// Reference: ACPI v6.3 Section 5.2.16.1.
    struct PACKED ProcessorAffinityEntry {
        EntryHeader header;
        u8 proximity_domain_low;
        u8 apic_id;
        Flag flags;
        u8 sapic_eid;
        u8 proximity_domain_high[3];
        u32 clock_domain;

        FORCE_INLINE CXX11_CONSTEXPR
        u32 proximity_domain() const {
            return u32(proximity_domain_low) |
                  (u32(proximity_domain_high[0]) << 8) |
                  (u32(proximity_domain_high[1]) << 16) |
                  (u32(proximity_domain_high[2]) << 24);
        }

        auto size() const -> usize {
            return header.length;
        }
    };
    static_assert(sizeof(ProcessorAffinityEntry) == 16);

    /// Type 1: memory affinity structure
    ///
    /// Reference: ACPI v6.3 Section 5.2.16.2.
    struct PACKED MemoryAffinityEntry {
        EntryHeader header;
        u32 proximity_domain;
        u16 _reserved;
        u32 base_address_low;
        u32 base_address_high;
        u32 length_low;
        u32 length_high;
        u32 _reserved2;
        Flag flags;
        u32 _reserved3;
        u32 _reserved4;

        auto size() const -> usize {
            return header.length;
        }
    };
    static_assert(sizeof(MemoryAffinityEntry) == 40);

    /// Type 2: processor x2apic affinity structure
    ///
    /// Reference: ACPI v6.3 Section 5.2.16.3.
    struct PACKED ProcessorX2ApicAffinityEntry {
        EntryHeader header;
        u16 _reserved;
        u32 proximity_domain;
        u32 x2apic_id;
        Flag flags;
        u32 clock_domain;
        u32 _reserved2;

        auto size() const -> usize {
            return header.length;
        }
    };
    static_assert(sizeof(ProcessorX2ApicAffinityEntry) == 24);

} // namespace acpi::srat

#endif // #ifndef ACPI_SRAT_HPP