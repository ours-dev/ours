#include <ours/arch/apic.hpp>
#include <ours/irq/irq_chip.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>

#include <array>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/x86/apic/ioapic.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/mem/align.hpp>

namespace ours {
namespace {
    using namespace irq;

    ustl::views::Span<arch::IoApic> IOAPICS;

    auto resolve_ioapic(usize num) -> arch::IoApic * {
        if (num >= IOAPICS.size()) {
            log::error("IO-APIC: Invalid IO-APIC number: {}", num);
            return nullptr;
        }
        return &IOAPICS[num];
    }

    struct IoApicChipData {
        struct PinNode: ustl::collections::intrusive::ListBaseHook<> {
            u8 apic_;
            u8 pin_;
        };

        bool isa_irq_;
        bool is_level_;
        bool active_high_;
        arch::IoApicRouteEntry entry;
        ustl::collections::intrusive::List<PinNode> pins_;
    };

    auto ioapic_mask_irq(IrqData &data) -> void {
        auto chip_data = static_cast<IoApicChipData *>(data.platform_data);
        for (auto &node: chip_data->pins_) {
            resolve_ioapic(node.apic_)->mask_irq(data.hirqnum);
        }
    }

    auto ioapic_unmask_irq(IrqData &data) -> void {
        auto chip_data = static_cast<IoApicChipData *>(data.platform_data);
        for (auto &node: chip_data->pins_) {
            resolve_ioapic(node.apic_)->unmask_irq(data.hirqnum);
        }
    }

    auto ioapic_startup_irq(IrqData &data) -> Status {
        ioapic_unmask_irq(data);
        return Status::Ok;
    }

    auto ioapic_issue_eoi(IrqData &data) -> void {
        auto chip_data = static_cast<IoApicChipData *>(data.platform_data);
        for (auto &node: chip_data->pins_) {
            // NotSupported from an IOAPIC older than version 0x20, which has
            // no EOI register; the pin's EOI is then simply not sent, where
            // Linux falls back to toggling the entry to edge and back
            // (docs/repairs/261003/x86-apic.md, item 1).
            auto _ = resolve_ioapic(node.apic_)->issue_eoi(data.hirqnum);
        }
    }

    auto ioapic_issue_ack(IrqData &data) -> void {
        ioapic_issue_eoi(data);
    }

    auto ioapic_set_affinity(IrqData &, CpuMask const *) -> Status {
        return Status::Ok;
    }

    auto ioapic_dump() -> void {
        for (auto &ioapic: IOAPICS) {
            log::info("IoApic[{}]:", ioapic.id());
            log::info("  version: {}", ioapic.version());
            log::info("  Addr: 0x{:X}", ioapic.phys_base());
            log::info("  MMIO: 0x{:X}", ioapic.virt_base());
            log::info("  GBI base: {}", ioapic.gsi_base());
            log::info("  Max local IRQ: {}\n", ioapic.max_local_irqnum());
        }
    }

    [[maybe_unused]]
    IrqChip IOAPIC_CHIP = {
        .startup = ioapic_startup_irq,
        .mask = ioapic_mask_irq,
        .unmask = ioapic_unmask_irq,
        .issue_eoi = ioapic_issue_eoi,
        .issue_ack = ioapic_issue_ack,
        .set_affinity = ioapic_set_affinity,
        .dump = ioapic_dump,
        .name = "IO-APIC",
    };
    std::array<arch::IoApicIsaOverride, arch::NUM_ISA_IRQS> IOAPIC_ISA_OVERRIDES;

    /// Finds the IO-APIC whose pins cover the global system interrupt
    /// `irqnum`: the one whose range [gsi_base, gsi_base + irq_count) holds
    /// it. The range lives in `IoApic::owns_global_irq`, next to the two
    /// numbers it is made of, so no caller has to remember that the version
    /// register reports the highest redirection index rather than the pin
    /// count, or that the pins are numbered from the IO-APIC's own base.
    auto resolve_irqnum_to_ioapic(HIrqNum irqnum) -> arch::IoApic * {
        for (auto &ioapic: IOAPICS) {
            if (ioapic.owns_global_irq(irqnum)) {
                return &ioapic;
            }
        }

        return nullptr;
    }
} // namespace

auto apic_mask_irq(HIrqNum global_irq) -> void {
    auto ioapic = resolve_irqnum_to_ioapic(global_irq);
    DEBUG_ASSERT(ioapic, "Out of valid irqnum range");
    ioapic->mask_irq(global_irq);
}

auto apic_unmask_irq(HIrqNum global_irq) -> void {
    auto ioapic = resolve_irqnum_to_ioapic(global_irq);
    DEBUG_ASSERT(ioapic, "Out of valid irqnum range");
    ioapic->unmask_irq(global_irq);
}

auto apic_configure_irq(HIrqNum global_irq, arch::ApicTriggerMode trimode,
                        arch::ApicInterruptPolarity polarity, arch::ApicDeliveryMode delmode,
                        bool mask, arch::ApicDestinationMode dstmode, u8 dst, arch::IrqVec vector)
    -> void {
    auto ioapic = resolve_irqnum_to_ioapic(global_irq);
    ASSERT(ioapic);

    ioapic->config_irq(global_irq, trimode, polarity, delmode, dstmode, dst, vector, mask);

    log::trace("IO-APIC[{}]: configuration({}, {}, {}, {}, {}, {})", ioapic->id(),
               to_string(trimode), to_string(polarity), to_string(delmode), to_string(dstmode), dst,
               usize(vector));
}

auto apic_configure_isa_irq(u8 isa_irq, arch::ApicDeliveryMode del_mode, bool mask,
                            arch::ApicDestinationMode dst_mode, u8 dst, arch::IrqVec vector)
    -> void {
    // The override table has one slot per ISA line; a bigger number is a
    // caller's bug, so stop here rather than read past the table.
    ASSERT(isa_irq < arch::NUM_ISA_IRQS, "ISA IRQ {} is past the {} ISA lines", isa_irq,
           arch::NUM_ISA_IRQS);
    HIrqNum irq = isa_irq;
    auto trigger_mode = arch::ApicTriggerMode::Edge;
    auto polarity = arch::ApicInterruptPolarity::ActiveHigh;
    if (IOAPIC_ISA_OVERRIDES[isa_irq].remapped) {
        irq = IOAPIC_ISA_OVERRIDES[isa_irq].global_irq;
        trigger_mode = IOAPIC_ISA_OVERRIDES[isa_irq].tri_mode;
        polarity = IOAPIC_ISA_OVERRIDES[isa_irq].polarity;
    }

    apic_configure_irq(irq, trigger_mode, polarity, del_mode, mask, dst_mode, dst, vector);
}

auto apic_io_isa_to_global(u8 isa_irq) -> HIrqNum {
    for (auto &override: IOAPIC_ISA_OVERRIDES) {
        if (override.isa_irq == isa_irq && override.remapped) {
            return override.global_irq;
        }
    }

    return isa_irq;
}

INIT_CODE
auto init_io_apic(ustl::views::Span<arch::IoApic> const &ioapics,
                  ustl::views::Span<arch::IoApicIsaOverride> const &overrides) -> void {
    auto const nr_ioapics = ioapics.size();
    {
        auto raw_ioapics = new (mm::GAF_KERNEL) arch::IoApic[nr_ioapics]();
        ASSERT(raw_ioapics, "Failed to allocate IO-APIC");
        IOAPICS = ustl::views::make_span(raw_ioapics, nr_ioapics);
    }

    for (auto i = usize{0}; i < nr_ioapics; ++i) {
        // Inplacement new to re-construct IO-APIC descriptor.
        IOAPICS[i] = ioapics[i];

        // Then need we create MMIO for per IoApic
        VirtAddr mmio_virt = 0;
        for (auto j = usize{0}; j < i; ++j) {
            if (IOAPICS[i].phys_base() == IOAPICS[j].phys_base()) {
                mmio_virt = IOAPICS[j].virt_base();
            }
        }

        // If no other shared IO-APICs, we have to create a new mapping for it.
        if (!mmio_virt) {
            auto const phys = IOAPICS[i].phys_base();
            auto const phys_aligned = ustl::mm::align_down(phys, PAGE_SIZE);

            auto rvma = mm::VmAspace::kernel_aspace()->root_vma();
            auto result = rvma->map_phys(
                phys_aligned, mm::VmPlacement::anywhere(), PAGE_SIZE,
                // `Discache` is required to prevent
                mm::MmuFlags::Discache | mm::MmuFlags::Readable | mm::MmuFlags::Writable,
                mm::VmMapOption::Populate | mm::VmMapOption::Pinned, "IO-APIC");
            DEBUG_ASSERT(result, "Failed to initialize IO-APIC");

            mmio_virt = (*result)->base() + (phys - phys_aligned);
        }

        IOAPICS[i].reset_mmio(mmio_virt);
    }

    // Every pin of every IO-APIC has to resolve back to it. The two ends of
    // each range are the cases a wrong bound gets wrong: the last pin when
    // the bound is off by one, the first when it forgets the IO-APIC's own
    // GSI base. A pin that resolves to another IO-APIC means the firmware
    // gave two of them overlapping ranges. This runs only once all of
    // IOAPICS is initialised: a default-constructed slot claims GSI 0.
    for (auto const &ioapic: IOAPICS) {
        [[maybe_unused]]
        auto const first = static_cast<HIrqNum>(ioapic.gsi_base());
        [[maybe_unused]]
        auto const last = static_cast<HIrqNum>(ioapic.gsi_base() + ioapic.irq_count() - 1);
        DEBUG_ASSERT(resolve_irqnum_to_ioapic(first) == &ioapic,
                     "IO-APIC[{}]: its first GSI {} does not resolve to it", ioapic.id(), first);
        DEBUG_ASSERT(resolve_irqnum_to_ioapic(last) == &ioapic,
                     "IO-APIC[{}]: its last GSI {} does not resolve to it", ioapic.id(), last);
    }

    log::info("ISA Irq | Remapped | Trigger mode | Polarity | Global IRQ");
    for (auto const &isa_override: overrides) {
        // The MADT is firmware's data, and the table below is indexed by the
        // ISA number it carries, so a number past the 16 ISA lines would be
        // a write past the end. Refuse that one entry and keep the rest, as
        // Linux's mp_override_legacy_irq() does.
        if (isa_override.isa_irq >= arch::NUM_ISA_IRQS) {
            log::warn("IO-APIC: ignoring an override of ISA IRQ {}, past the {} ISA lines",
                      isa_override.isa_irq, arch::NUM_ISA_IRQS);
            continue;
        }

        // Stored at its ISA number, where apic_configure_isa_irq() looks.
        IOAPIC_ISA_OVERRIDES[isa_override.isa_irq] = isa_override;

        log::trace("{: <6} | {} | {} | {} | {}", isa_override.isa_irq, isa_override.remapped,
                   to_string(isa_override.tri_mode), to_string(isa_override.polarity),
                   isa_override.global_irq);
    }
}

} // namespace ours
