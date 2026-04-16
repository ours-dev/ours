/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_MADT_HPP
#define ACPI_MADT_HPP 1

#include <acpi/types.hpp>
#include <acpi/structures/header.hpp>

#include <ustl/io/binary_reader.hpp>

namespace acpi::madt {
    enum class Type: u8 {
        LocalApic,
        IoApic,
        InterruptOverride,
        Nmi,
        LocalApicNmi,
        IoSapic,
        LocalSapic,
        PlatformInterruptSource,
        LocalX2Apic,
        IoX2Apic,
        LocalX2ApicNmi,
        GicCpuInterface,
        GicDistributor,
        GicMsiFrame,
        GicRedistributor,
        GicExtendedRegister,
        Count,
    };

    enum class Flag {
        PolarityConforms = 0,
        PolarityHigh = 0b01,
        PolarityLow = 0b11,
        PolarityMask = 0b11,

        TriggerConforms = 0b0000,
        TriggerEdge = 0b0100,
        TriggerLevel = 0b1100,
        TriggerMask = 0b1100,
    };

    /// Multiple APIC Description Table
    struct PACKED Table {
        TableHeader header;
        u32 lapic_addr;
        u32 flags;

        CXX11_CONSTEXPR
        static auto const kSignature = Signature("APIC");
    };

    // MADT Entry Type 0: Processor Local APIC
    struct PACKED LocalApicEntry {
        EntryHeader header;
        u8 acpi_processor_id;
        u8 apic_id;

        // Flags (bit 0 = Processor Enabled) (bit 1 = Online Capable)
        u32 flags; 

        static constexpr auto kType = Type::LocalApic;
    };

    // MADT Entry Type 1: I/O APIC
    struct PACKED IoApicEntry {
        EntryHeader header;
        u8 io_apic_id;        // I/O APIC ID
        u8 reserved;          // Reserved
        u32 io_apic_address;  // I/O APIC address
        u32 global_irq_base;  // Global interrupt base

        static constexpr auto kType = Type::IoApic;
    };

    // MADT Entry Type 2: Interrupt Override
    struct PACKED InterruptOverrideEntry {
        EntryHeader header;
        u8 bus;               // Bus number
        u8 source;            // Interrupt source number
        u32 global_irq;       // Global interrupt number
        u8 flags;             // Flags

        static constexpr auto kType = Type::InterruptOverride;
    };

    // MADT Entry Type 3: NMI Interrupt
    struct PACKED NmiEntry {
        EntryHeader header;
        u8 acpi_processor_id; // ACPI processor ID
        u16 flags;            // Flags
        u8 global_irq;        // Global interrupt number

        static constexpr auto kType = Type::Nmi;
    };

    // MADT Entry Type 4: Local APIC NMI
    struct PACKED LocalApicNmiEntry {
        EntryHeader header;
        u8 acpi_processor_id; // ACPI processor ID
        u16 flags;            // Flags
        u8 local_apic_lint;   // Local APIC LINT pin

        static constexpr auto kType = Type::LocalApicNmi;
    };

    // MADT Entry Type 5: I/O SAPIC
    struct PACKED IoSapicEntry {
        EntryHeader header;
        u8 acpi_processor_id; // ACPI processor ID
        u8 io_sapic_id;       // I/O SAPIC ID
        u32 flags;            // Flags
        u32 io_sapic_address; // I/O SAPIC address

        static constexpr auto kType = Type::IoSapic;
    };

    // MADT Entry Type 6: Local SAPIC
    struct PACKED LocalSapicEntry {
        EntryHeader header;
        u8 acpi_processor_id; // ACPI processor ID
        u8 local_sapic_id;    // Local SAPIC ID
        u8 local_sapic_eid;   // Local SAPIC EID
        u32 flags;            // Flags
        u32 reserved[2];      // Reserved

        static constexpr auto kType = Type::LocalSapic;
    };

    // MADT Entry Type 7: Platform Interrupt Source
    struct PACKED PlatformInterruptSourceEntry {
        EntryHeader header;
        u8 type;              // Interrupt source type
        u8 reserved;          // Reserved
        u16 flags;            // Flags
        u32 global_irq;       // Global interrupt number

        static constexpr auto kType = Type::PlatformInterruptSource;
    };

    // MADT Entry Type 8: Processor Local x2APIC
    struct PACKED LocalX2ApicEntry {
        EntryHeader header;
        u8 reserved[3];       // Reserved
        u8 apic_id;           // x2APIC ID
        u32 flags;            // Flags
        u32 acpi_processor_id;// ACPI processor ID

        static constexpr auto kType = Type::LocalX2Apic;
    };

    // MADT Entry Type 9: I/O x2APIC
    struct PACKED IoX2ApicEntry {
        EntryHeader header;
        u8 reserved[3];       // Reserved
        u8 io_apic_id;        // I/O x2APIC ID
        u32 flags;            // Flags
        u32 io_apic_address;  // I/O x2APIC address
        u32 global_irq_base;  // Global interrupt base

        static constexpr auto kType = Type::IoX2Apic;
    };

    // MADT Entry Type 10: Local x2APIC NMI
    struct PACKED LocalX2ApicNmiEntry {
        EntryHeader header;
        u8 reserved[3];       // Reserved
        u8 apic_id;           // x2APIC ID
        u16 flags;            // Flags
        u8 local_apic_lint;   // Local x2APIC LINT pin

        static constexpr auto kType = Type::LocalX2ApicNmi;
    };

    // MADT Entry Type 11: GIC CPU Interface
    struct PACKED GicCpuInterfaceEntry {
        EntryHeader header;
        u8 acpi_processor_id; // ACPI processor ID
        u8 flags;             // Flags
        u16 reserved;         // Reserved
        u32 gic_cpu_interface_number; // GIC CPU interface number

        static constexpr auto kType = Type::GicCpuInterface;
    };

    // MADT Entry Type 12: GIC Distributor
    struct PACKED GicDistributorEntry {
        EntryHeader header;
        u8 reserved[3];       // Reserved
        u8 gic_distributor_id;// GIC Distributor ID
        u32 flags;            // Flags
        u32 gic_distributor_base_address; // GIC Distributor base address

        static constexpr auto kType = Type::GicDistributor;
    };

    // MADT Entry Type 13: GIC MSI Frame
    struct PACKED GicMsiFrameEntry {
        EntryHeader header;
        u8 reserved[3];       // Reserved
        u8 gic_msi_frame_id;  // GIC MSI Frame ID
        u32 flags;            // Flags
        u32 gic_msi_frame_base_address; // GIC MSI Frame base address
        u32 reserved2;        // Reserved

        static constexpr auto kType = Type::GicMsiFrame;
    };

    // MADT Entry Type 14: GIC Redistributor
    struct PACKED GicRedistributorEntry {
        EntryHeader header;
        u8 reserved[3];       // Reserved
        u8 gic_redistributor_id; // GIC Redistributor ID
        u32 flags;            // Flags
        u32 gic_redistributor_base_address; // GIC Redistributor base address
        u32 reserved2;        // Reserved

        static constexpr auto kType = Type::GicRedistributor;
    };

    // MADT Entry Type 15: GIC Extended Register
    struct PACKED GicExtendedRegisterEntry {
        EntryHeader header;
        u8 reserved[3];       // Reserved
        u8 gic_extended_register_id; // GIC Extended Register ID
        u32 flags;            // Flags
        u32 gic_extended_register_base_address; // GIC Extended Register base address
        u32 reserved2;        // Reserved

        static constexpr auto kType = Type::GicExtendedRegister;
    };

} // namespace acpi::madt

#endif // #ifndef ACPI_MADT_HPP