#include <ours/arch/apic.hpp>
#include <ours/irq/irq_chip.hpp>
#include <ours/mem/vm_aspace.hpp>
#include <ours/mem/vm_area.hpp>
#include <ours/mem/vm_mapping.hpp>

#include <arch/x86/apic/ioapic.hpp>
#include <arch/auto-interrupt-disabler.hpp>

#include <ustl/mem/box.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ktl/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using namespace irq;

    ustl::views::Span<arch::IoApic>  s_ioapics;

    auto resolve_ioapic(usize num) -> arch::IoApic * {
        if (num >= s_ioapics.size()) {
            log::error("IO-APIC: Invalid IO-APIC number: {}", num);
            return nullptr;
        }
        return &s_ioapics[num];
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
        for (auto &node : chip_data->pins_) {
            resolve_ioapic(node.apic_)->mask_irq(data.hirqnum);
        }
    }

    auto ioapic_unmask_irq(IrqData &data) -> void {
        auto chip_data = static_cast<IoApicChipData *>(data.platform_data);
        for (auto &node : chip_data->pins_) {
            resolve_ioapic(node.apic_)->unmask_irq(data.hirqnum);
        }
    }

    auto ioapic_startup_irq(IrqData &data) -> Status {
        ioapic_unmask_irq(data);
        return Status::Ok;
    }

    auto ioapic_issue_eoi(IrqData &data) -> void {
        auto chip_data = static_cast<IoApicChipData *>(data.platform_data);
        for (auto &node : chip_data->pins_) {
            resolve_ioapic(node.apic_)->issue_eoi(data.hirqnum);
        }
    }

    auto ioapic_issue_ack(IrqData &data) -> void {
        ioapic_issue_eoi(data);
    }

    auto ioapic_set_affinity(IrqData &data, CpuMask const *desc) -> Status {
        auto chip_data = static_cast<IoApicChipData *>(data.platform_data);
        return Status::Ok;
    }

    auto ioapic_dump() -> void {
        for (auto &ioapic: s_ioapics) {
            log::info("IoApic[{}]:", ioapic.id());
            log::info("  version: {}", ioapic.version());
            log::info("  Addr: 0x{:X}", ioapic.phys_base());
            log::info("  MMIO: 0x{:X}", ioapic.virt_base());
            log::info("  GBI base: {}", ioapic.gsi_base());
            log::info("  Max local IRQ: {}\n", ioapic.max_local_irqnum());
        }
    }

    IrqChip s_ioapic_chip = {
        .startup = ioapic_startup_irq,
        .mask = ioapic_mask_irq,
        .unmask = ioapic_unmask_irq,
        .issue_eoi = ioapic_issue_eoi,
        .issue_ack = ioapic_issue_ack,
        .set_affinity = ioapic_set_affinity,
        .dump = ioapic_dump,
        .name = "IO-APIC",
    };
    ustl::Array<arch::IoApicIsaOverride, arch::kNumIsaIrqs> s_ioapic_isa_overrides;

    auto resolve_irqnum_to_ioapic(HIrqNum irqnum) -> arch::IoApic * {
        for (auto i = 0; i < s_ioapics.size(); ++i) {
            if (s_ioapics[i].gsi_base() <= irqnum && 
                irqnum < s_ioapics[i].max_local_irqnum()) {
                return &s_ioapics[i];
            }
        }

        return 0;
    }
} // namespace anonymous

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
                            bool mask, arch::ApicDestinationMode dstmode, u8 dst, arch::IrqVec vector) -> void {
        auto ioapic = resolve_irqnum_to_ioapic(global_irq);
        ASSERT(ioapic);

        ioapic->config_irq(
            global_irq, 
            trimode, 
            polarity, 
            delmode, 
            dstmode, 
            dst, 
            vector,
            mask
        );

        log::trace("IO-APIC[{}]: configuration({}, {}, {}, {}, {}, {})", 
            ioapic->id(),
            to_string(trimode),
            to_string(polarity),
            to_string(delmode),
            to_string(dstmode),
            dst,
            usize(vector)
        );
    }

    auto apic_configure_isa_irq(u8 isa_irq, arch::ApicDeliveryMode del_mode, bool mask,
                                arch::ApicDestinationMode dst_mode, u8 dst, arch::IrqVec vector) -> void {
        HIrqNum irq = isa_irq;
        auto trigger_mode = arch::ApicTriggerMode::Edge; 
        auto polarity = arch::ApicInterruptPolarity::ActiveHigh; 
        if (s_ioapic_isa_overrides[isa_irq].remapped) {
            irq = s_ioapic_isa_overrides[isa_irq].global_irq;
            trigger_mode = s_ioapic_isa_overrides[isa_irq].tri_mode;
            polarity = s_ioapic_isa_overrides[isa_irq].polarity;
        }

        apic_configure_irq(irq, trigger_mode, polarity, del_mode, mask, dst_mode, dst, vector);
    }

    auto apic_io_isa_to_global(u8 isa_irq) -> HIrqNum {
        for (auto &override: s_ioapic_isa_overrides) {
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
            auto raw_ioapics = new (mem::kGafKernel) arch::IoApic[nr_ioapics]();
            ASSERT(raw_ioapics, "Failed to allocate IO-APIC");
            s_ioapics = ustl::views::make_span(raw_ioapics, nr_ioapics);
        }

        for (auto i = 0; i < nr_ioapics; ++i) {
            // Inplacement new to re-construct IO-APIC descriptor.
            s_ioapics[i] = ioapics[i];

            // Then need we create MMIO for per IoApic
            VirtAddr mmio_virt = 0;
            for (auto j = 0; j < i; ++i) {
                if (s_ioapics[i].phys_base() ==
                    s_ioapics[j].phys_base()) {
                    mmio_virt = s_ioapics[j].virt_base();
                }
            }

            // If no other shared IO-APICs, we have to create a new mapping for it.
            if (!mmio_virt) {
                auto const phys = s_ioapics[i].phys_base();
                auto const phys_aligned = ustl::mem::align_down(phys, PAGE_SIZE);

                using namespace mem;
                auto rvma = VmAspace::kernel_aspace()->root_vma();
                auto result = rvma->map_at(phys_aligned, 0, PAGE_SIZE, 
                    // `Discache` is required to prevent 
                    MmuFlags::Discache | MmuFlags::Readable | MmuFlags::Writable, 
                    VmMapOption::Commit | VmMapOption::Pinned, "IO-APIC"
                );
                DEBUG_ASSERT(result, "Failed to initialize IO-APIC");

                mmio_virt = (*result)->base() + (phys - phys_aligned);
            }

            s_ioapics[i].reset_mmio(mmio_virt);
        }

        log::info("ISA Irq | Remapped | Trigger mode | Polarity | Global IRQ");
        for (auto i = 0; i < nr_overrides; ++i) {
            /// Make a linear mapping
            s_ioapic_isa_overrides[overrides[i].isa_irq] = overrides[i];

            log::trace("{: <6} | {} | {} | {} | {}", 
                s_ioapic_isa_overrides[i].isa_irq, 
                s_ioapic_isa_overrides[i].remapped, 
                to_string(s_ioapic_isa_overrides[i].tri_mode),
                to_string(s_ioapic_isa_overrides[i].polarity),
                s_ioapic_isa_overrides[i].global_irq
            );
        }
    }

} // namespace ours