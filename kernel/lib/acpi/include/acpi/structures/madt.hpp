/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_STRUCTURES_MADT_HPP
#define ACPI_STRUCTURES_MADT_HPP 1

#include <acpi/structures/header.hpp>
#include <ustl/util/enum_bits.hpp>

namespace acpi::madt {
/// Interrupt controller structure types.
///
/// Reference: ACPI v6.5 Table 5.20.
///
/// The previous spelling of this enum omitted `LocalApicAddressOverride` (0x05)
/// and invented an `IoX2Apic` that the specification does not define.  The two
/// mistakes cancelled at 0x0A, so 0x05 through 0x09 all named the wrong
/// structure while the tail happened to look correct.
enum class Type : u8 {
    LocalApic = 0x00,
    IoApic = 0x01,
    InterruptSourceOverride = 0x02,
    NmiSource = 0x03,
    LocalApicNmi = 0x04,
    LocalApicAddressOverride = 0x05,
    IoSapic = 0x06,
    LocalSapic = 0x07,
    PlatformInterruptSources = 0x08,
    LocalX2Apic = 0x09,
    LocalX2ApicNmi = 0x0A,
    GicCpuInterface = 0x0B,
    GicDistributor = 0x0C,
    GicMsiFrame = 0x0D,
    GicRedistributor = 0x0E,
    GicInterruptTranslationService = 0x0F,
    MultiprocessorWakeup = 0x10,
};

/// MPS INTI flags, carried by the interrupt source override and NMI entries.
///
/// Reference: ACPI v6.5 Table 5.26.
///
/// These are two-bit *fields*, not independent bits, so they are decoded with
/// accessors rather than exposed as a bitmask enum -- `flags & PolarityLow`
/// is true for `PolarityHigh` as well, which is exactly the trap the old
/// `madt::Flag` enum set up.
enum class Polarity : u8 {
    Conforms = 0b00,
    ActiveHigh = 0b01,
    Reserved = 0b10,
    ActiveLow = 0b11,
};

enum class TriggerMode : u8 {
    Conforms = 0b00,
    Edge = 0b01,
    Reserved = 0b10,
    Level = 0b11,
};

[[nodiscard]]
constexpr auto polarity_of(u16 inti_flags) -> Polarity {
    return Polarity(inti_flags & 0b11);
}

[[nodiscard]]
constexpr auto trigger_mode_of(u16 inti_flags) -> TriggerMode {
    return TriggerMode((inti_flags >> 2) & 0b11);
}

/// Reference: ACPI v6.5 Table 5.23.
enum class LocalApicFlag : u32 {
    Enabled = 0x01,
    OnlineCapable = 0x02,
};
USTL_ENABLE_ENUM_BITMASK(LocalApicFlag);

/// Multiple APIC description table.
///
/// Reference: ACPI v6.5 Section 5.2.12.
struct PACKED Table {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return header.length;
    }

    TableHeader header;
    u32 lapic_addr;
    u32 flags;

    constexpr static Signature SIGNATURE = Signature::from_chars("APIC");
};
static_assert(sizeof(Table) == 44);

/// Type 0x00. Reference: ACPI v6.5 Section 5.2.12.2.
struct PACKED LocalApicEntry {
    EntryHeader header;
    u8 acpi_processor_uid;
    u8 apic_id;
    LocalApicFlag flags;

    constexpr static Type TYPE = Type::LocalApic;
};
static_assert(sizeof(LocalApicEntry) == 8);

/// Type 0x01. Reference: ACPI v6.5 Section 5.2.12.3.
struct PACKED IoApicEntry {
    EntryHeader header;
    u8 io_apic_id;
    u8 reserved;
    u32 io_apic_address;
    u32 global_irq_base;

    constexpr static Type TYPE = Type::IoApic;
};
static_assert(sizeof(IoApicEntry) == 12);

/// Type 0x02. Reference: ACPI v6.5 Section 5.2.12.5.
///
/// `flags` is a WORD in the specification.  It used to be declared `u8`, which
/// made the structure 9 bytes instead of 10; it only appeared to work because
/// the polarity and trigger fields live in the low byte on little-endian
/// targets.
struct PACKED InterruptSourceOverrideEntry {
    [[nodiscard]]
    constexpr auto polarity() const -> Polarity {
        return polarity_of(flags);
    }

    [[nodiscard]]
    constexpr auto trigger_mode() const -> TriggerMode {
        return trigger_mode_of(flags);
    }

    EntryHeader header;
    u8 bus;
    u8 source;
    u32 global_irq;
    u16 flags;

    constexpr static Type TYPE = Type::InterruptSourceOverride;
};
static_assert(sizeof(InterruptSourceOverrideEntry) == 10);

/// Type 0x03. Reference: ACPI v6.5 Section 5.2.12.6.
///
/// This is the NMI *source* structure: flags then a global system interrupt.
/// The previous definition carried a processor id and a one byte interrupt,
/// which is the shape of the Local APIC NMI structure (type 0x04).
struct PACKED NmiSourceEntry {
    [[nodiscard]]
    constexpr auto polarity() const -> Polarity {
        return polarity_of(flags);
    }

    [[nodiscard]]
    constexpr auto trigger_mode() const -> TriggerMode {
        return trigger_mode_of(flags);
    }

    EntryHeader header;
    u16 flags;
    u32 global_irq;

    constexpr static Type TYPE = Type::NmiSource;
};
static_assert(sizeof(NmiSourceEntry) == 8);

/// Type 0x04. Reference: ACPI v6.5 Section 5.2.12.7.
struct PACKED LocalApicNmiEntry {
    EntryHeader header;
    u8 acpi_processor_uid;
    u16 flags;
    u8 local_apic_lint;

    constexpr static Type TYPE = Type::LocalApicNmi;
};
static_assert(sizeof(LocalApicNmiEntry) == 6);

/// Type 0x05. Reference: ACPI v6.5 Section 5.2.12.8.
///
/// Missing entirely from the previous definitions, which is what shifted every
/// following type number by one.
struct PACKED LocalApicAddressOverrideEntry {
    EntryHeader header;
    u16 reserved;
    u64 local_apic_address;

    constexpr static Type TYPE = Type::LocalApicAddressOverride;
};
static_assert(sizeof(LocalApicAddressOverrideEntry) == 12);

/// Type 0x06. Reference: ACPI v6.5 Section 5.2.12.9.
struct PACKED IoSapicEntry {
    EntryHeader header;
    u8 io_apic_id;
    u8 reserved;
    u32 global_irq_base;
    u64 io_sapic_address;

    constexpr static Type TYPE = Type::IoSapic;
};
static_assert(sizeof(IoSapicEntry) == 16);

/// Type 0x07. Reference: ACPI v6.5 Section 5.2.12.10.
///
/// Variable length: a NUL terminated ACPI processor UID string follows the
/// fixed part, so `header.length` may exceed `sizeof(LocalSapicEntry)`.
struct PACKED LocalSapicEntry {
    EntryHeader header;
    u8 acpi_processor_id;
    u8 local_sapic_id;
    u8 local_sapic_eid;
    u8 reserved[3];
    LocalApicFlag flags;
    u32 acpi_processor_uid;

    constexpr static Type TYPE = Type::LocalSapic;
};
static_assert(sizeof(LocalSapicEntry) == 16);

/// Type 0x08. Reference: ACPI v6.5 Section 5.2.12.11.
struct PACKED PlatformInterruptSourcesEntry {
    EntryHeader header;
    u16 flags;
    u8 interrupt_type;
    u8 processor_id;
    u8 processor_eid;
    u8 io_sapic_vector;
    u32 global_irq;
    u32 platform_interrupt_source_flags;

    constexpr static Type TYPE = Type::PlatformInterruptSources;
};
static_assert(sizeof(PlatformInterruptSourcesEntry) == 16);

/// Type 0x09. Reference: ACPI v6.5 Section 5.2.12.12.
///
/// `x2apic_id` is a DWORD.  It used to be declared `u8` behind a three byte
/// reserved field, making the structure 14 bytes instead of 16 and reading the
/// wrong offset for every field after it.
struct PACKED LocalX2ApicEntry {
    EntryHeader header;
    u16 reserved;
    u32 x2apic_id;
    LocalApicFlag flags;
    u32 acpi_processor_uid;

    constexpr static Type TYPE = Type::LocalX2Apic;
};
static_assert(sizeof(LocalX2ApicEntry) == 16);

/// Type 0x0A. Reference: ACPI v6.5 Section 5.2.12.13.
struct PACKED LocalX2ApicNmiEntry {
    EntryHeader header;
    u16 flags;
    u32 acpi_processor_uid;
    u8 local_x2apic_lint;
    u8 reserved[3];

    constexpr static Type TYPE = Type::LocalX2ApicNmi;
};
static_assert(sizeof(LocalX2ApicNmiEntry) == 12);

// Types 0x0B through 0x0F (the GIC family) are deliberately not defined here.
//
// They used to be, but every one of those definitions was fabricated -- the GIC
// CPU interface structure is 80 bytes in ACPI 6.4 and 82 in 6.5, and the
// declaration here claimed 10.  Nothing in this tree consumes them: the library
// is only wired into the x86 platform and arch paths today.  A wrong layout is
// worse than an absent one, so whoever brings up ARM ACPI should add these back
// against the specification rather than inherit these.  The `Type` enumerators
// above are complete regardless, so type dispatch stays correct.

} // namespace acpi::madt

#endif // #ifndef ACPI_STRUCTURES_MADT_HPP
