#include <ours/arch/apic.hpp>
#include <ours/assert.hpp>
#include <ours/irq/chip.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/sync/spin_lock.hpp>

#include <array>

#include <arch/x86/apic/ioapic.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

namespace ours {
namespace {
    using namespace irq;

    /// The IO-APICs the MADT lists. The span and every descriptor in it are
    /// written once, by init_io_apic(), and only read afterwards, so they are
    /// read without a lock. Their registers are another matter: see
    /// IOAPIC_LOCK.
    ustl::views::Span<arch::IoApic> IOAPICS;

    /// LI, the IO-APIC register lock of
    /// `.agents/tasks/microkernel-interrupts/DESIGN.md`. Every access to an
    /// IO-APIC's registers in this file happens under it: each IOREGSEL and
    /// IOWIN pair, each read-modify-write of a redirection entry, the EOI
    /// register.
    ///
    /// The pair is why: a register is read by writing its index to IOREGSEL
    /// and then reading IOWIN, and there is one IOREGSEL per IO-APIC. Without
    /// the lock, CPU0 masking one line in an interrupt handler and CPU1
    /// unmasking another in a system call interleave as select(A), select(B),
    /// window -> B: CPU0 reads, or worse writes, the other line's entry. A
    /// mask or unmask is then two such pairs around a modification, so even
    /// on one line two unlocked read-modify-writes lose one another's change.
    ///
    /// One lock for all IO-APICs, not one each: a machine has one or two,
    /// and a mask is a few MMIO accesses, so a finer lock would buy nothing
    /// measurable while adding a lock to choose on every path. IrqSave,
    /// because mask() is called from interrupt context: a thread holding it
    /// with interrupts on, interrupted on its own CPU by a handler that masks,
    /// would spin on itself. It is the innermost lock of the interrupt path:
    /// L1 -> L2 -> LI; nothing is taken under it.
    // Namespace-scope objects are UPPER_CASE throughout this tree.
    // NOLINTNEXTLINE(readability-identifier-naming)
    sync::SpinLock IOAPIC_LOCK;

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

    /// The IO-APIC that drives a line of the line table. Every line there was
    /// programmed at boot through apic_configure_irq(), which stops on a GSI
    /// no IO-APIC owns, so a miss here is a kernel bug: stop rather than
    /// dereference null.
    auto owner_of(HIrqNum const gsi) -> arch::IoApic & {
        auto *const ioapic = resolve_irqnum_to_ioapic(gsi);
        ASSERT(ioapic, "IO-APIC: no IO-APIC owns GSI {}", gsi);
        return *ioapic;
    }

    // The chip's operations. Each resolves the IO-APIC from the line's GSI,
    // `data.hirqnum`: a GSI names one IO-APIC and one of its pins, so a line
    // needs no chip data of its own, and `data.platform_data` stays null.
    // A line wired to several pins would need such data; no line is.

    auto ioapic_mask_irq(irq::Data const &data) -> void {
        auto &ioapic = owner_of(data.hirqnum);
        Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
        ioapic.mask_irq(data.hirqnum);
    }

    auto ioapic_unmask_irq(irq::Data const &data) -> void {
        auto &ioapic = owner_of(data.hirqnum);
        Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
        ioapic.unmask_irq(data.hirqnum);
    }

    auto ioapic_startup_irq(irq::Data const &data) -> Status {
        ioapic_unmask_irq(data);
        return Status::Ok;
    }

    /// The vector, delivery mode and destination stay as platform_init_apic()
    /// programmed them: a line's vector is fixed by its GSI. Only how the pin
    /// signals changes. The pin is left masked: a line changes how it
    /// signals only while nobody is on it, and the driver that attaches next
    /// unmasks it once its handler is in place.
    auto ioapic_configure_irq(irq::Data const &data, TriggerMode const trigger,
                              Polarity const polarity) -> Status {
        auto apic_trigger = arch::ApicTriggerMode::Edge;
        if (trigger == TriggerMode::Level) {
            apic_trigger = arch::ApicTriggerMode::Level;
        }
        auto apic_polarity = arch::ApicInterruptPolarity::ActiveHigh;
        if (polarity == Polarity::ActiveLow) {
            apic_polarity = arch::ApicInterruptPolarity::ActiveLow;
        }

        auto &ioapic = owner_of(data.hirqnum);
        Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
        ioapic.reconfigure_irq(data.hirqnum, apic_trigger, apic_polarity);
        return Status::Ok;
    }

    /// Read from the pin's redirection entry, under LI like every register
    /// access: the entry is the one record of how the pin signals, written
    /// by the platform at boot and by ioapic_configure_irq() since.
    auto ioapic_trigger_mode(irq::Data const &data) -> TriggerMode {
        auto &ioapic = owner_of(data.hirqnum);
        auto apic_trigger = arch::ApicTriggerMode::Edge;
        {
            Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
            apic_trigger = ioapic.trigger_mode(data.hirqnum);
        }
        if (apic_trigger == arch::ApicTriggerMode::Level) {
            return TriggerMode::Level;
        }
        return TriggerMode::Edge;
    }

    /// The mask bit of the pin's redirection entry, read under LI like every
    /// register access. The entry is the one record of whether the pin is
    /// masked: mask and unmask write it, and nothing keeps a copy. Only the
    /// kernel tests read it, to see what a line's flow and its owner leave
    /// there.
    auto ioapic_is_masked(irq::Data const &data) -> bool {
        auto &ioapic = owner_of(data.hirqnum);
        Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
        return ioapic.is_masked(data.hirqnum);
    }

    /// The chip's end of interrupt, which a line's flow sends once the
    /// actions have run: the local APIC's EOI, which takes the vector out of
    /// this CPU's in-service register. For a level-triggered vector the
    /// local APIC also sends the EOI on to the IO-APICs, and the entry that
    /// carries the vector clears its Remote IRR, so the pin can deliver
    /// again. No IO-APIC register is written, so LI is not taken.
    auto ioapic_eoi(irq::Data const &) -> void {
        apic_issue_eoi();
    }

    /// The IO-APIC's own EOI register, an EOI aimed at one IO-APIC rather
    /// than sent on by the local APIC. Unused: the chip's issue_eoi is
    /// ioapic_eoi(), and this is reached only through issue_ack, which
    /// nothing calls. It also writes the GSI where the vector belongs
    /// (docs/repairs/261003/x86-apic.md, item 2).
    auto ioapic_issue_eoi(irq::Data const &data) -> void {
        auto &ioapic = owner_of(data.hirqnum);
        Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
        // NotSupported from an IOAPIC older than version 0x20, which has
        // no EOI register; the pin's EOI is then simply not sent. Such a
        // chip needs the entry toggled to edge and back instead, which is
        // not implemented (docs/repairs/261003/x86-apic.md, item 1).
        auto _ = ioapic.issue_eoi(data.hirqnum);
    }

    auto ioapic_issue_ack(irq::Data const &data) -> void {
        ioapic_issue_eoi(data);
    }

    // The platform aims every pin at the boot CPU when it programs it, and
    // nothing moves one afterwards.
    auto ioapic_set_affinity(irq::Data const &, CpuMask const *) -> Status {
        return Status::NotSupported;
    }

    /// Reads only what reset_mmio() cached, no register: no lock.
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

    constexpr irq::Chip IOAPIC_CHIP = {
        .startup = ioapic_startup_irq,
        .mask = ioapic_mask_irq,
        .unmask = ioapic_unmask_irq,
        .configure = ioapic_configure_irq,
        .trigger_mode = ioapic_trigger_mode,
        .is_masked = ioapic_is_masked,
        .issue_eoi = ioapic_eoi,
        .issue_ack = ioapic_issue_ack,
        .set_affinity = ioapic_set_affinity,
        .dump = ioapic_dump,
        .name = "IO-APIC",
    };
    std::array<arch::IoApicIsaOverride, arch::NUM_ISA_IRQS> IOAPIC_ISA_OVERRIDES;
} // namespace

auto io_apic_chip() -> irq::Chip const & {
    return IOAPIC_CHIP;
}

auto registered_io_apics() -> ustl::views::Span<arch::IoApic const> {
    return IOAPICS;
}

auto apic_configure_irq(HIrqNum global_irq, arch::ApicTriggerMode trimode,
                        arch::ApicInterruptPolarity polarity, arch::ApicDeliveryMode delmode,
                        bool mask, arch::ApicDestinationMode dstmode, u8 dst, arch::IrqVec vector)
    -> void {
    auto &ioapic = owner_of(global_irq);
    {
        Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
        ioapic.config_irq(global_irq, trimode, polarity, delmode, dstmode, dst, vector, mask);
    }

    log::trace("IO-APIC[{}]: configuration({}, {}, {}, {}, {}, {})", ioapic.id(),
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
            auto const phys_aligned = ustl::round_down(phys, PAGE_SIZE);

            auto rvma = mm::VmAspace::kernel_aspace()->root_vma();
            auto result = rvma->map_phys(
                phys_aligned, mm::VmPlacement::anywhere(), PAGE_SIZE,
                // `Discache` is required to prevent
                mm::MmuFlags::Discache | mm::MmuFlags::Readable | mm::MmuFlags::Writable,
                mm::VmMapOption::Populate | mm::VmMapOption::Pinned, "IO-APIC");
            DEBUG_ASSERT(result, "Failed to initialize IO-APIC");

            mmio_virt = (*result)->base() + (phys - phys_aligned);
        }

        // reset_mmio() reads the ID and version registers: a register
        // access like any other, so under LI too, although nothing else can
        // reach the IO-APICs this early.
        Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
        IOAPICS[i].reset_mmio(mmio_virt);
    }

    // Every pin of every IO-APIC has to resolve back to it. The two ends of
    // each range are the cases a wrong bound gets wrong: the last pin when
    // the bound is off by one, the first when it forgets the IO-APIC's own
    // GSI base. A pin that resolves to another IO-APIC means the firmware
    // gave two of them overlapping ranges. It has to come after
    // reset_mmio(): until that reads the version register, a slot's maximum
    // redirection index is 0, so its last GSI is its first and the check
    // of the last pin would test nothing.
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
        // a write past the end. Refuse that one entry and keep the rest.
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
