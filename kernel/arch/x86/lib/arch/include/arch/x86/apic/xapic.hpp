/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_APIC_XAPIC_HPP
#define ARCH_X86_APIC_XAPIC_HPP 1

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/types.hpp>
#include <arch/x86/apic/common.hpp>
#include <arch/x86/msr.hpp>

namespace arch {
enum class XApicRegType : u32 {
    Id = 0x020,
    Version = 0x030,
    Eoi = 0x0B0,
    LogicalDestination = 0x0D0,
    SpuriousVector = 0x0F0,
    Esr = 0x280,
    IcrLow = 0x300,
    IcrHigh = 0x310,
    LvtTimer = 0x320,
    LvtPmi = 0x340,
    LvtLogicalInt0 = 0x350,
    LvtError = 0x370,
    TimerInitCount = 0x380,
    TimerCurrentCount = 0x390,
    TimerDivConf = 0x3E0,
};

inline constexpr auto INVALID_APIC_ID = u32{0xFFFF'FFFF};
inline constexpr auto APIC_PHYS_BASE = PhysAddr{0xFEE0'0000};
inline constexpr auto IA32_APIC_BASE_BSP = usize{1} << 8;
inline constexpr auto IA32_APIC_BASE_X2_APIC_ENABLE = usize{1} << 10;
inline constexpr auto IA32_APIC_BASE_XAPIC_ENABLE = usize{1} << 11;

class XApic final {
  public:
    enum class TscMode : u32 {
        OneShot = 0,
        Periodic = 1,
        Deadline = 2,
    };

    auto init(volatile u32 *mmio_base) noexcept -> void {
        mmio_base_ = mmio_base;
    }

    [[nodiscard]]
    auto id() const noexcept -> u32 {
        return read_reg(XApicRegType::Id) >> 24;
    }

    [[nodiscard]]
    auto logical_id() const noexcept -> u32 {
        return read_reg(XApicRegType::LogicalDestination) >> 24;
    }

    [[nodiscard]]
    auto version() const noexcept -> u32 {
        return read_reg(XApicRegType::Version) & 0xFF;
    }

    auto enable_tsc(IrqVec vector, TscMode mode = TscMode::OneShot) noexcept -> void {
        update_reg(XApicRegType::LvtTimer, VECTOR_MASK | TIMER_MODE_MASK | INTERRUPT_MASK,
                   (static_cast<u32>(vector) & VECTOR_MASK) | (static_cast<u32>(mode) << 17));
    }

    auto disable_tsc(IrqVec vector) noexcept -> void {
        update_reg(XApicRegType::LvtTimer, VECTOR_MASK,
                   (static_cast<u32>(vector) & VECTOR_MASK) | INTERRUPT_MASK);
    }

    auto set_tsc_deadline(u64 deadline) noexcept -> void {
        MsrIo::write(MsrRegAddr::IA32TscDeadline, deadline);
    }

    [[nodiscard]]
    auto tsc_deadline_enabled() const noexcept -> bool {
        return (read_reg(XApicRegType::LvtTimer) & TIMER_MODE_MASK) == (u32{2} << 17);
    }

    /// Programs the spurious-interrupt vector register: the vector on which a
    /// spurious interrupt is reported, and bit 8, which is the local APIC's
    /// *software enable*.
    ///
    /// Bit 8 is not a detail. An x86 core comes out of reset -- and out of
    /// INIT/SIPI -- with SVR = 0x0000'00FF, software-disabled, and a
    /// software-disabled local APIC delivers nothing at all: no IPI, no LVT,
    /// no timer. Only the boot CPU escapes this by accident, because the
    /// firmware enables its APIC before handing over (QEMU's OVMF leaves
    /// SVR = 0x10F on the BSP and 0xFF on every AP, which is exactly what the
    /// boot log showed).
    ///
    /// Linux writes the same word in setup_local_APIC(),
    /// `APIC_SPIV_APIC_ENABLED | SPURIOUS_APIC_VECTOR`, on every CPU
    /// including the boot one.
    auto enable(IrqVec spurious_vector) noexcept -> void {
        update_reg(XApicRegType::SpuriousVector, VECTOR_MASK,
                   (static_cast<u32>(spurious_vector) & VECTOR_MASK) | SOFTWARE_ENABLE);
    }

    auto issue_eoi() noexcept -> void {
        write_reg(XApicRegType::Eoi, 0);
    }

    auto send_ipi(IpiRequest const &request) noexcept -> void {
        AutoInterruptDisabler guard;
        write_reg(XApicRegType::IcrHigh, request.icr_high());
        write_reg(XApicRegType::IcrLow, request.icr_low());
        wait_for_ipi();
    }

    auto send_ipi(u32, IpiRequest const &request) noexcept -> void {
        send_ipi(request);
    }

    auto send_ipi_self(IpiRequest request) noexcept -> void {
        request.set_target(ApicIpiTarget::Self);
        send_ipi(request);
    }

    auto broadcast_ipi(IpiRequest request) noexcept -> void {
        request.set_target(ApicIpiTarget::All);
        send_ipi(request);
    }

    auto broadcast_ipi_without_self(IpiRequest request) noexcept -> void {
        request.set_target(ApicIpiTarget::AllButSelf);
        send_ipi(request);
    }

    auto mask(XApicRegType type) noexcept -> void {
        update_reg(type, 0, INTERRUPT_MASK);
    }

    auto unmask(XApicRegType type) noexcept -> void {
        update_reg(type, INTERRUPT_MASK, 0);
    }

    /// Read-modify-write of one APIC register: clear the |clear| bits, then
    /// set the |set| bits.
    ///
    /// Every write to an APIC register in this class is that shape -- an LVT
    /// entry is a vector packed beside mode and mask bits that the write must
    /// preserve -- and it had been spelled out longhand five times. Naming it
    /// also names the ordering: clear first, then set, so a caller that lists
    /// a bit in both means "set it".
    auto update_reg(XApicRegType type, u32 clear, u32 set) noexcept -> void {
        write_reg(type, (read_reg(type) & ~clear) | set);
    }

    [[nodiscard]]
    auto read_reg(XApicRegType type) const noexcept -> u32 {
        return *register_address(type);
    }

    template <typename Value>
    auto write_reg(XApicRegType type, Value value) noexcept -> void {
        *register_address(type) = static_cast<u32>(value);
    }

  private:
    [[nodiscard]]
    auto register_address(XApicRegType type) const noexcept -> volatile u32 * {
        return reinterpret_cast<volatile u32 *>(reinterpret_cast<volatile u8 *>(mmio_base_) +
                                                static_cast<u32>(type));
    }

    auto wait_for_ipi() const noexcept -> void {
        while ((read_reg(XApicRegType::IcrLow) & (u32{1} << 12)) != 0 &&
               read_reg(XApicRegType::Esr) == 0) {
        }
    }

    static constexpr auto VECTOR_MASK = u32{0xFF};
    static constexpr auto INTERRUPT_MASK = u32{1} << 16;
    static constexpr auto TIMER_MODE_MASK = u32{3} << 17;

    /// SVR bit 8: the local APIC's software enable. Clear after reset.
    static constexpr auto SOFTWARE_ENABLE = u32{1} << 8;

    volatile u32 *mmio_base_{};
};
} // namespace arch

#endif // ARCH_X86_APIC_XAPIC_HPP
