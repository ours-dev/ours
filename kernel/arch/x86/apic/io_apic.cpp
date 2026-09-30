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
            resolve_ioapic(node.apic_)->issue_eoi(data.hirqnum);
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

    auto resolve_irqnum_to_ioapic(HIrqNum irqnum) -> arch::IoApic * {
        for (auto i = usize{0}; i < IOAPICS.size(); ++i) {
            if (IOAPICS[i].gsi_base() <= irqnum && irqnum < IOAPICS[i].max_local_irqnum()) {
                return &IOAPICS[i];
            }
        }

        return 0;
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
    auto const nr_overrides = overrides.size();
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

    log::info("ISA Irq | Remapped | Trigger mode | Polarity | Global IRQ");
    for (auto i = usize{0}; i < nr_overrides; ++i) {
        /// Make a linear mapping
        IOAPIC_ISA_OVERRIDES[overrides[i].isa_irq] = overrides[i];

        log::trace("{: <6} | {} | {} | {} | {}", IOAPIC_ISA_OVERRIDES[i].isa_irq,
                   IOAPIC_ISA_OVERRIDES[i].remapped, to_string(IOAPIC_ISA_OVERRIDES[i].tri_mode),
                   to_string(IOAPIC_ISA_OVERRIDES[i].polarity), IOAPIC_ISA_OVERRIDES[i].global_irq);
    }
}

} // namespace ours
