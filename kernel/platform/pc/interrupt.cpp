#include <ours/arch/apic.hpp>
#include <ours/irq/domain.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/mod.hpp>
#include <ours/platform/acpi.hpp>
#include <ours/platform/isa.hpp>
#include <ours/platform/mmio.hpp>

#include <algorithm>

#include <acpi/structures/madt.hpp>
#include <acpi/table_set.hpp>
#include <arch/interrupt.hpp>
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

    /// Static controller entry for the root domain. It owns the reader scope
    /// for each dispatch; the platform never borrows domain storage.
    irq::DomainEndpoint ROOT_ENDPOINT;

    /// The root handler of a PC, installed by platform_init_apic(). Every pin
    /// of every IO-APIC was programmed with vector PlatformIrqMin + GSI, so
    /// the GSI is the vector less that base. The irq layer looks the line up by
    /// the GSI; every other part names the line by its virq.
    ///
    /// A line's flow ends its own interrupt, through the line's chip, once the
    /// actions have run; that holds for an IPI sent to a line's vector too. A
    /// vector no line has, a pin the platform did not program or an IPI sent
    /// to such a vector, is ended here instead: left in service, it would hold
    /// back every interrupt of its priority class and below on this CPU for
    /// good.
    auto handle_root_interrupt(arch::IrqFrame *const frame) -> void {
        DEBUG_ASSERT(frame->vector >= arch::IrqVec::PlatformIrqMin);
        auto const gsi = static_cast<HIrqNum>(static_cast<u32>(frame->vector) -
                                              static_cast<u32>(arch::IrqVec::PlatformIrqMin));
        if (ROOT_ENDPOINT.dispatch(gsi).is_none()) {
            apic_issue_eoi();
        }
    }

    INIT_CODE
    auto platform_init_apic() -> void {
        // Before enable the APIC, we should shutdown the PIC.
        arch::Pic::disable();

        auto tables = get_acpi_tables();
        DEBUG_ASSERT(tables, "No ACPI tables");
        auto const *madt = tables->find<acpi::madt::Table>();
        DEBUG_ASSERT(madt, "No MADT");

        // The kernel maps and drives the local APIC and every IO-APIC, a page
        // each; no MmIo resource grants those pages.
        platform::reserve_kernel_mmio(arch::APIC_PHYS_BASE, PAGE_SIZE);

        ktl::Vec<arch::IoApic> io_apics;
        for (auto const &entry: acpi::entries<acpi::madt::IoApicEntry>(*madt)) {
            auto slot = io_apics.emplace_back();
            ASSERT(slot, "Out of memory while collecting IO-APICs from the MADT");
            slot.unwrap()->init(entry.io_apic_address,
                                mm::phys_to_pfn(PhysAddr(entry.io_apic_address)),
                                entry.global_irq_base);
            platform::reserve_kernel_mmio(entry.io_apic_address, PAGE_SIZE);
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

        // Every pin of every IO-APIC is programmed, masked, at its GSI with
        // vector PlatformIrqMin + GSI: first the pins the ISA lines reach, as
        // the MADT's overrides send them, then all the others. The root handler
        // turns a vector back into a number by subtracting the same base, so
        // what it hands the irq layer is the GSI, by which the layer looks the
        // line up; every other part names the line by its virq.
        //
        // The vector is fixed by the GSI, not taken from a pool of free
        // vectors when the first handler registers on a line. One IO-APIC's
        // 24 pins fit the 208 platform vectors with room to spare; allocating
        // them waits for MSI, which brings more.
        constexpr auto MAX_GSI = usize(IrqVec::PlatformIrqMax) - usize(IrqVec::PlatformIrqMin);
        ktl::Vec<HIrqNum> gsis;
        auto const is_programmed = [&gsis](HIrqNum const gsi) -> bool {
            auto const taken = ustl::views::Span(gsis);
            return std::ranges::find(taken, gsi) != taken.end();
        };
        auto const remember = [&gsis](HIrqNum const gsi) -> void {
            auto slot = gsis.emplace_back(gsi);
            ASSERT(slot, "Out of memory while collecting the GSIs of the IO-APIC pins");
        };
        for (auto isa = IsaIrqNum{0}; isa < NUM_ISA_IRQS; ++isa) {
            // ISA 2 is the cascade between the two PICs and carries no device.
            // QEMU sends ISA 0 to GSI 2 without moving ISA 2 off it, so
            // programming ISA 2 would overwrite the PIT's entry.
            if (isa == ISA_IRQ_PIC2) {
                continue;
            }

            auto const gsi = apic_io_isa_to_global(isa);
            // Two ISA lines on one GSI would program one entry twice and be
            // one line, which register_domain() refuses. The first keeps the GSI.
            // Mapping the overrides first would instead let a line moved onto
            // a pin beat the line whose own number it is; on QEMU the only
            // such pair is ISA 0 and 2, which the check above settles the
            // same way.
            if (is_programmed(gsi)) {
                log::warn("ISA IRQ {} shares GSI {} with an earlier ISA line; not programmed",
                          u32(isa), static_cast<u32>(gsi));
                continue;
            }

            ASSERT(static_cast<usize>(gsi) <= MAX_GSI,
                   "GSI {} of ISA IRQ {} is past the platform vectors", static_cast<u32>(gsi),
                   u32(isa));
            auto const vector = IrqVec(usize(IrqVec::PlatformIrqMin) + static_cast<usize>(gsi));
            apic_configure_isa_irq(isa, ApicDeliveryMode::Fixed, true,
                                   ApicDestinationMode::Physical, static_cast<u8>(bspid), vector);
            remember(gsi);
        }

        // Then every pin no ISA line reached, which on a PC are the pins PCI
        // wires its interrupts to. They are programmed the way PCI signals,
        // level and active low; whoever takes a line says otherwise with a
        // mode, which configures the pin as the line is taken. The entry is
        // masked, so nothing arrives before a driver unmasks its line. A pin
        // whose GSI is past the platform vectors gets no line and is left as
        // the firmware left it.
        for (auto const &ioapic: registered_io_apics()) {
            for (auto pin = usize{0}; pin < ioapic.irq_count(); ++pin) {
                auto const gsi = static_cast<HIrqNum>(ioapic.gsi_base() + pin);
                if (is_programmed(gsi)) {
                    continue;
                }

                if (static_cast<usize>(gsi) > MAX_GSI) {
                    log::warn("IO-APIC[{}] pin {} is GSI {}, past the platform vectors; no line",
                              ioapic.id(), pin, static_cast<u32>(gsi));
                    continue;
                }

                auto const vector = IrqVec(usize(IrqVec::PlatformIrqMin) + static_cast<usize>(gsi));
                apic_configure_irq(gsi, ApicTriggerMode::Level, ApicInterruptPolarity::ActiveLow,
                                   ApicDeliveryMode::Fixed, true, ApicDestinationMode::Physical,
                                   static_cast<u8>(bspid), vector);
                remember(gsi);
            }
        }

        // The irq layer's lines are the GSIs just programmed, and could not be
        // made before the MADT said which those are. They are made before the
        // first unmask, which is a driver's, after this function.
        //
        // Each line is bound to the IO-APIC's chip as it is made, so a driver
        // masks and unmasks its line through the irq layer, and the chip
        // takes the IO-APIC register lock; no driver touches the IO-APIC. The
        // lines are the machine's root domain, and their table is published
        // through ROOT_ENDPOINT, which the handler above dispatches through.
        auto const status = irq::register_domain(irq::DomainId::Root, ustl::views::Span(gsis),
                                                 io_apic_controller(), ROOT_ENDPOINT);
        ASSERT(Status::Ok == status, "Failed to build the interrupt lines: {}", to_string(status));

        // The root controller's driver gives arch its handler now, with the
        // lines built and the pins still masked, and before the instruction
        // below that lets an interrupt in.
        irq::set_root_handler(handle_root_interrupt);

        arch::local_irq_enable();
    }
    // Before every kernel driver that uses a line: the PIT, which attaches to
    // its line at Irq + 1, and the HPET, which masks that line across its
    // switch at Irq + 2, find the lines this builds. A user-space driver's
    // interrupt_create comes long after.
    GKTL_INIT_HOOK(ApicInit, platform_init_apic, gktl::InitLevel::Irq);
} // namespace

/// The same mapping platform_init_apic() programmed each ISA line at, so the
/// line a driver names this way is the one its device's interrupts reach: the
/// ISA number gives a GSI, and the irq layer says which line was made for that
/// GSI. The bound comes first: apic_io_isa_to_global() takes a u8, and a
/// number past the ISA lines would wrap into one of them. The cascade is left
/// out: platform_init_apic() does not program it, and its GSI is commonly the
/// timer's (q35 sends ISA 0 to GSI 2 and leaves ISA 2 on it), so naming it
/// would hand out the timer's line under a second name.
auto platform::isa_irq_line(u32 const isa_irq) -> VIrqNum {
    if (isa_irq >= arch::NUM_ISA_IRQS || isa_irq == ISA_IRQ_PIC2) {
        return VIrqNum::Invalid;
    }
    return irq::irq_for_source(irq::DomainId::Root,
                               apic_io_isa_to_global(static_cast<u8>(isa_irq)));
}
} // namespace ours