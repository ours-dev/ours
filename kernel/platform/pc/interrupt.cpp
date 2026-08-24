#include <acpi/structures/madt.hpp>
#include <acpi/table_set.hpp>
#include <arch/x86/apic/ioapic.hpp>
#include <arch/x86/apic/xapic.hpp>
#include <arch/x86/pic.hpp>
#include <gktl/init_hook.hpp>
#include <ktl/vec.hpp>
#include <ours/arch/apic.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/mod.hpp>
#include <ours/platform/acpi.hpp>
#include <ours/platform/isa.hpp>

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
            io_apics.emplace_back().init(entry.io_apic_address,
                                         mm::phys_to_pfn(PhysAddr(entry.io_apic_address)),
                                         entry.global_irq_base);
        }

        ktl::Vec<arch::IoApicIsaOverride> io_apic_isa_overrides;
        for (auto const &entry: acpi::entries<acpi::madt::InterruptSourceOverrideEntry>(*madt)) {
            io_apic_isa_overrides.emplace_back(parse_io_apic_isa_overrides(entry));
        }

        init_local_apic();
        init_io_apic(ustl::views::Span(io_apics), ustl::views::Span(io_apic_isa_overrides));

        // Build the map between CpuNum and APIC
        u32 num_local_apics = 0;
        for (auto const &entry: acpi::entries<acpi::madt::LocalApicEntry>(*madt)) {
            auto const cpunum = assign_cpunum_for_apic(entry.apic_id);
            DEBUG_ASSERT(num_local_apics <= MAX_CPU && kInvalidCpuNum != cpunum);
            ++num_local_apics;
        }

        using namespace arch;
        auto const bspid = current_apic_id();
        // Manually configing the ISA IRQs
        for (auto i = 0; i < 8; ++i) {
            // Regardless of IRQ 2
            constexpr static auto const kPicCascadeIrqNum = 2;
            if (i != kPicCascadeIrqNum) {
                auto const irqnum = IrqVec(usize(IrqVec::PlatformIrqMin) + i);
                apic_configure_isa_irq(i, ApicDeliveryMode::Fixed, true,
                                       ApicDestinationMode::Physical, bspid, irqnum);
            }

            auto const irqnum = IrqVec(usize(IrqVec::PlatformIrqMin) + i + 8);
            apic_configure_isa_irq(i + 8, ApicDeliveryMode::Fixed, true,
                                   ApicDestinationMode::Physical, bspid, irqnum);
        }

        arch::local_irq_enable();
    }
    // Let PIT driver run before us.
    GKTL_INIT_HOOK(ApicInit, platform_init_apic, gktl::InitLevel::Irq);
} // namespace

auto platform_handle_irq(HIrqNum irqnum, arch::IrqFrame *frame) -> void {
    irq::handle_irq_generic(irqnum);
}

auto platform_handle_irq_preamble(HIrqNum irqnum, arch::IrqFrame *frame) -> void {
    irq::handle_irq_preamble(irqnum, frame);
}

auto platform_handle_irq_postamble(HIrqNum irqnum, arch::IrqFrame *) -> bool {
    return irq::handle_irq_postamble();
}
} // namespace ours