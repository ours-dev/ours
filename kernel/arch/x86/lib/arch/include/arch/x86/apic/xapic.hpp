/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_APIC_XAPIC_HPP
#define ARCH_X86_APIC_XAPIC_HPP 1

#include <arch/auto-interrupt-disabler.hpp>
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

inline constexpr auto kInvalidApicId = u32{0xFFFF'FFFF};
inline constexpr auto kApicPhysBase = PhysAddr{0xFEE0'0000};
inline constexpr auto kIa32ApicBaseBsp = usize{1} << 8;
inline constexpr auto kIa32ApicBaseX2ApicEnable = usize{1} << 10;
inline constexpr auto kIa32ApicBaseXapicEnable = usize{1} << 11;

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
        auto value = read_reg(XApicRegType::LvtTimer);
        value &= ~(kVectorMask | kTimerModeMask | kInterruptMask);
        value |= static_cast<u32>(vector) & kVectorMask;
        value |= static_cast<u32>(mode) << 17;
        write_reg(XApicRegType::LvtTimer, value);
    }

    auto disable_tsc(IrqVec vector) noexcept -> void {
        auto value = read_reg(XApicRegType::LvtTimer);
        value = (value & ~kVectorMask) | (static_cast<u32>(vector) & kVectorMask) | kInterruptMask;
        write_reg(XApicRegType::LvtTimer, value);
    }

    auto set_tsc_deadline(u64 deadline) noexcept -> void {
        MsrIo::write(MsrRegAddr::IA32TscDeadline, deadline);
    }

    [[nodiscard]]
    auto tsc_deadline_enabled() const noexcept -> bool {
        return (read_reg(XApicRegType::LvtTimer) & kTimerModeMask) == (u32{2} << 17);
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
        write_reg(type, read_reg(type) | kInterruptMask);
    }

    auto unmask(XApicRegType type) noexcept -> void {
        write_reg(type, read_reg(type) & ~kInterruptMask);
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

    static constexpr auto kVectorMask = u32{0xFF};
    static constexpr auto kInterruptMask = u32{1} << 16;
    static constexpr auto kTimerModeMask = u32{3} << 17;

    volatile u32 *mmio_base_{};
};
} // namespace arch

#endif // ARCH_X86_APIC_XAPIC_HPP
