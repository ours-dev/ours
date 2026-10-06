#include <ours/arch/apic.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/mod.hpp>
#include <ours/platform/acpi.hpp>
#include <ours/platform/isa.hpp>

#include <algorithm>
#include <array>

#include <acpi/structures/madt.hpp>
#include <acpi/table_set.hpp>
#include <arch/x86/apic/ioapic.hpp>
#include <arch/x86/apic/xapic.hpp>
#include <arch/x86/pic.hpp>
#include <gktl/init_hook.hpp>
#include <ktl/vec.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    INIT_CODE
    auto parse_io_apic_isa_overrides(acpi::madt::InterruptSourceOverrideEntry const &entry)
        -> arch::IoApicIsaOverride {
        arch::IoApicIsaOverride object;
        // Polarity and trigger mode are two-bit fields inside the MPS INTI
        // word, not independent flag bits; acpi::madt decodes them rather than
        // handing out masks to be AND-ed by hand.
        switch (entry.trigger_mode()) {
            case acpi::madt::TriggerMode::Conforms:
            case acpi::madt::TriggerMode::Edge:
                object.tri_mode = arch::ApicTriggerMode::Edge;
                break;
            case acpi::madt::TriggerMode::Level:
                object.tri_mode = arch::ApicTriggerMode::Level;
                break;
            default:
                panic("Unknown IRQ trigger mode: {}.", u32(entry.trigger_mode()));
        }

        switch (entry.polarity()) {
            case acpi::madt::Polarity::Conforms:
            case acpi::madt::Polarity::ActiveHigh:
                object.polarity = arch::ApicInterruptPolarity::ActiveHigh;
                break;
            case acpi::madt::Polarity::ActiveLow:
                object.polarity = arch::ApicInterruptPolarity::ActiveLow;
                break;
            default:
                panic("Unknown IRQ interrupt polarity: {}.", u32(entry.polarity()));
        }

        object.global_irq = entry.global_irq;
        object.remapped = true;
        object.isa_irq = entry.source;
        return object;
    }

    INIT_CODE
    auto platform_init_apic() -> void {
        // Before enable the APIC, we should shutdown the PIC.
        arch::Pic::disable();

        auto tables = get_acpi_tables();
        DEBUG_ASSERT(tables, "No ACPI tables");
        auto const *madt = tables->find<acpi::madt::Table>();
        DEBUG_ASSERT(madt, "No MADT");

        ktl::Vec<arch::IoApic> io_apics;
        for (auto const &entry: acpi::entries<acpi::madt::IoApicEntry>(*madt)) {
            auto slot = io_apics.emplace_back();
            ASSERT(slot, "Out of memory while collecting IO-APICs from the MADT");
            slot.unwrap()->init(entry.io_apic_address,
                                mm::phys_to_pfn(PhysAddr(entry.io_apic_address)),
                                entry.global_irq_base);
        }

        ktl::Vec<arch::IoApicIsaOverride> io_apic_isa_overrides;
        for (auto const &entry: acpi::entries<acpi::madt::InterruptSourceOverrideEntry>(*madt)) {
            auto slot = io_apic_isa_overrides.emplace_back(parse_io_apic_isa_overrides(entry));
            ASSERT(slot, "Out of memory while collecting IO-APIC ISA overrides from the MADT");
        }

        init_local_apic();
        init_io_apic(ustl::views::Span(io_apics), ustl::views::Span(io_apic_isa_overrides));

        // Build the map between CpuNum and APIC
        [[maybe_unused]]
        u32 num_local_apics = 0;
        for (auto const &entry: acpi::entries<acpi::madt::LocalApicEntry>(*madt)) {
            [[maybe_unused]]
            auto const cpunum = assign_cpunum_for_apic(entry.apic_id);
            DEBUG_ASSERT(num_local_apics <= MAX_CPU && INVALID_CPU_NUM != cpunum);
            ++num_local_apics;
        }

        using namespace arch;
        auto const bspid = current_apic_id();

        // Every ISA line is programmed, masked, at its GSI -- the IO-APIC pin
        // the MADT's overrides send it to -- with vector PlatformIrqMin + GSI.
        // faults.cpp turns a vector back into a number by subtracting the same
        // base, so what reaches platform_handle_irq() is the GSI, the same
        // number the line is attached by and masked by. One line, one number.
        //
        // The vector is fixed by the GSI, not taken from a pool of free
        // vectors when the first handler registers on a line. One IO-APIC's
        // 24 pins fit the 208 platform vectors with room to spare; allocating
        // them waits for PCI and MSI, which bring more.
        constexpr auto MAX_GSI = usize(IrqVec::PlatformIrqMax) - usize(IrqVec::PlatformIrqMin);
        std::array<HIrqNum, NUM_ISA_IRQS> gsis{};
        auto nr_gsis = usize{0};
        for (auto isa = IsaIrqNum{0}; isa < NUM_ISA_IRQS; ++isa) {
            // ISA 2 is the cascade between the two PICs and carries no device.
            // QEMU sends ISA 0 to GSI 2 without moving ISA 2 off it, so
            // programming ISA 2 would overwrite the PIT's entry.
            if (isa == ISA_IRQ_PIC2) {
                continue;
            }

            auto const gsi = apic_io_isa_to_global(isa);
            // Two ISA lines on one GSI would program one entry twice and be
            // one line, which init_lines() refuses. The first keeps the GSI.
            // Mapping the overrides first would instead let a line moved onto
            // a pin beat the line whose own number it is; on QEMU the only
            // such pair is ISA 0 and 2, which the check above settles the
            // same way.
            auto const taken = ustl::views::Span(gsis).first(nr_gsis);
            if (std::ranges::find(taken, gsi) != taken.end()) {
                log::warn("ISA IRQ {} shares GSI {} with an earlier ISA line; not programmed",
                          u32(isa), gsi);
                continue;
            }

            ASSERT(gsi <= MAX_GSI, "GSI {} of ISA IRQ {} is past the platform vectors", gsi,
                   u32(isa));
            auto const vector = IrqVec(usize(IrqVec::PlatformIrqMin) + gsi);
            apic_configure_isa_irq(isa, ApicDeliveryMode::Fixed, true,
                                   ApicDestinationMode::Physical, static_cast<u8>(bspid), vector);
            gsis[nr_gsis] = gsi;
            ++nr_gsis;
        }

        // The irq layer's lines are the GSIs just programmed, and could not be
        // made before the MADT said which those are. They are made before the
        // first unmask, which is a driver's, after this function. A line for
        // every GSI of every IO-APIC waits for PCI, the first user of a GSI
        // that no ISA line reaches.
        //
        // Each line is bound to the IO-APIC's chip as it is made, so a driver
        // masks and unmasks its line through the irq layer, and the chip
        // takes the IO-APIC register lock; no driver touches the IO-APIC.
        auto const status = irq::init_lines(ustl::views::Span(gsis).first(nr_gsis), io_apic_chip());
        ASSERT(Status::Ok == status, "Failed to build the interrupt lines: {}", to_string(status));

        arch::local_irq_enable();
    }
    // Before every kernel driver that uses a line: the PIT, which attaches to
    // its line at Irq + 1, and the HPET, which masks that line across its
    // switch at Irq + 2, find the lines this builds. A user-space driver's
    // interrupt_create comes long after.
    GKTL_INIT_HOOK(ApicInit, platform_init_apic, gktl::InitLevel::Irq);
} // namespace

/// `irqnum` is the GSI: the vector less PlatformIrqMin, and platform_init_apic()
/// programmed every GSI with vector PlatformIrqMin + GSI. The irq layer names
/// its lines by the same number, so it passes through unchanged.
auto platform_handle_irq(HIrqNum irqnum, arch::IrqFrame *frame) -> bool {
    return irq::handle_irq_generic(irqnum);
}

auto platform_start_handling_irq(HIrqNum irqnum, arch::IrqFrame *frame) -> void {
    irq::handle_irq_preamble(irqnum, frame);
}

auto platform_finish_handling_irq(HIrqNum irqnum, arch::IrqFrame *) -> bool {
    return irq::handle_irq_postamble();
}

/// The same mapping platform_init_apic() programmed each ISA line at, so the
/// line a driver names this way is the one its device's interrupts reach.
/// The bound comes first: apic_io_isa_to_global() takes a u8, and a number
/// past the ISA lines would wrap into one of them rather than stay itself.
auto platform::remap_isa_irq(u32 const irq) -> VIrqNum {
    if (irq >= arch::NUM_ISA_IRQS) {
        return irq;
    }
    return apic_io_isa_to_global(static_cast<u8>(irq));
}
} // namespace ours