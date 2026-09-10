/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_APIC_HPP
#define OURS_ARCH_APIC_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/irq/irq_chip.hpp>

#include <arch/x86/apic/ioapic.hpp>
#include <arch/x86/apic/xapic.hpp>
#include <arch/x86/interrupt.hpp>
#include <ustl/views/span.hpp>

namespace ours {
using arch::ApicDeliveryMode;
using arch::ApicDestinationMode;
using arch::ApicTriggerMode;

extern arch::IoApic *IOAPIC;
extern arch::XApic *XAPIC;

INIT_CODE
auto init_io_apic(ustl::views::Span<arch::IoApic> const &ioapics,
                  ustl::views::Span<arch::IoApicIsaOverride> const &overrides) -> void;

/// Called once after VMM initialized.
INIT_CODE
auto init_local_apic() -> void;

INIT_CODE
auto init_local_apic_percpu() -> void;

INIT_CODE
auto init_apic_deadline_tsc() -> void;

auto apic_issue_eoi() -> void;

auto apic_handle_timer_irq() -> void;

auto apic_mask_irq(HIrqNum global_irq) -> void;

auto apic_unmask_irq(HIrqNum global_irq) -> void;

auto apic_send_ipi_self(arch::IrqVec vector, ApicDeliveryMode mode) -> void;

auto apic_send_ipi_unicast(CpuNum target, arch::IrqVec vector, ApicDeliveryMode mode) -> void;

auto apic_send_ipi_mulitcast(CpuMask const &targets, arch::IrqVec vector, ApicDeliveryMode mode)
    -> void;

auto apic_send_ipi_broadcast(arch::IrqVec vector, ApicDeliveryMode mode) -> void;

auto apic_configure_irq(HIrqNum global_irq, arch::ApicTriggerMode, arch::ApicInterruptPolarity,
                        arch::ApicDeliveryMode, bool mask, arch::ApicDestinationMode, u8 dst,
                        arch::IrqVec vector) -> void;

auto apic_configure_isa_irq(u8 isa_irq, arch::ApicDeliveryMode del_mode, bool mask,
                            arch::ApicDestinationMode dst_mode, u8 dst, arch::IrqVec vector)
    -> void;

auto apic_io_isa_to_global(u8 isa_irq) -> HIrqNum;

INIT_CODE
auto assign_cpunum_for_apic(u32 apicid) -> CpuNum;

INIT_CODE
auto current_apic_id_early() -> u32;

auto current_apic_id() -> u32;

auto cpunum_to_apic_id(CpuNum cpunum) -> u32;

auto apic_id_to_cpunum(u32 apicid) -> CpuNum;

} // namespace ours

#endif // #ifndef OURS_ARCH_APIC_HPP
