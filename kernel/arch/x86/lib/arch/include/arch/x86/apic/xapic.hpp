/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_APIC_XAPIC_HPP
#define ARCH_X86_APIC_XAPIC_HPP 1

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
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

    /// send_ipi(), but it gives up rather than hanging on an ICR that never
    /// goes idle. Returns false when it gave up, in which case nothing was
    /// written and no IPI was sent.
    ///
    /// Linux keeps the same pair -- __xapic_wait_icr_idle() beside
    /// safe_apic_wait_icr_idle() -- and reaches for the bounded one in exactly
    /// the places where a wedged APIC must not take the sender with it: NMI
    /// delivery, and starting a CPU. Those are the paths with nobody left to
    /// report the hang.
    [[nodiscard]]
    auto try_send_ipi(IpiRequest const &request) noexcept -> bool {
        AutoInterruptDisabler guard;
        if (!try_wait_icr_idle()) {
            return false;
        }
        write_reg(XApicRegType::IcrHigh, request.icr_high());
        write_reg(XApicRegType::IcrLow, request.icr_low());
        return true;
    }

    auto issue_eoi() noexcept -> void {
        write_reg(XApicRegType::Eoi, 0);
    }

    /// Issues one IPI.
    ///
    /// The ICR is two registers and the order matters: IcrHigh (0x310) only
    /// parks the destination APIC ID, while the write to IcrLow (0x300) is
    /// what actually sends. Both writes have to happen with interrupts off,
    /// or a handler that sends an IPI of its own could land between them and
    /// leave this one addressed to the wrong CPU.
    ///
    /// The idle wait is *before* the writes, not after. What the hardware
    /// requires is that the ICR be idle when it is next written; it does not
    /// require the sender to watch its own IPI land. Waiting afterwards --
    /// which is what this used to do -- bought nothing and put the delivery
    /// latency of a message meant for another CPU on this one's critical
    /// path, on a wakeup path that every cross-CPU unpark now walks. Moved to
    /// the front, the check is free in the common case (a single IPI with any
    /// gap after it finds the ICR already idle) and only actually waits when
    /// two sends really are back to back, which is the one case the rule is
    /// about.
    ///
    /// It has to be inside the interrupt disabler, not before it: waiting
    /// outside would let an interrupt issue its own IPI in the gap and make
    /// the ICR busy again just as this one starts writing.
    ///
    /// Linux does the same, in __default_send_IPI_dest_field(): "Wait for
    /// idle", then write ICR2, then ICR.
    auto send_ipi(IpiRequest const &request) noexcept -> void {
        AutoInterruptDisabler guard;
        wait_icr_idle();
        write_reg(XApicRegType::IcrHigh, request.icr_high());
        write_reg(XApicRegType::IcrLow, request.icr_low());
    }

    /// Sends |request| to each APIC id in |dests|, one after another.
    ///
    /// Physical destination mode addresses exactly one APIC per write, so a
    /// multicast really is N sends and the two register writes per target are
    /// irreducible. What is not irreducible is everything around them, and
    /// this used to pay all of it N times: a fresh IpiRequest built field by
    /// field when only the destination differs, and its own pushf/cli/popf
    /// around each one.
    ///
    /// One interrupt-disabled window for the whole set, as Linux's
    /// default_send_IPI_mask_sequence_phys() does -- it takes local_irq_save()
    /// once and loops inside. The window is longer, which is the trade being
    /// made: N sends of a handful of MMIO writes, against N times the cost of
    /// entering and leaving it.
    template <typename Dests>
    auto send_ipi_multicast(IpiRequest request, Dests const &dests) noexcept -> void {
        AutoInterruptDisabler guard;
        for (auto const dest: dests) {
            wait_icr_idle();
            request.set_dest(dest);
            write_reg(XApicRegType::IcrHigh, request.icr_high());
            write_reg(XApicRegType::IcrLow, request.icr_low());
        }
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

    /// Spins until the ICR is idle, i.e. until any previously issued IPI has
    /// been accepted by its destination.
    ///
    /// Only the delivery-status bit is read. The loop used to read the error
    /// status register each time round as well, as an escape hatch, and that
    /// was worth nothing twice over: ESR only latches a new value after a
    /// write to it, so reading it bare returns whatever was left from some
    /// earlier error and never reports this one -- while doubling the
    /// uncached MMIO traffic of every iteration. A stale non-zero ESR would
    /// have broken the loop early and let the next write clobber an ICR that
    /// was still busy, which is the exact failure the wait exists to prevent.
    ///
    /// Unbounded, like Linux's __xapic_wait_icr_idle(). Linux keeps a bounded
    /// safe_apic_wait_icr_idle() for the NMI and CPU-bring-up paths, where a
    /// wedged ICR must not take the sender down with it; there is no such
    /// variant here yet.
    auto wait_icr_idle() const noexcept -> void {
        while ((read_reg(XApicRegType::IcrLow) & ICR_DELIVERY_STATUS_PENDING) != 0) {
            spin_hint();
        }
    }

    /// The bounded form. See try_send_ipi(), its only caller.
    [[nodiscard]]
    auto try_wait_icr_idle() const noexcept -> bool {
        for (auto spin = usize{0}; spin < MAX_ICR_SPINS; ++spin) {
            if ((read_reg(XApicRegType::IcrLow) & ICR_DELIVERY_STATUS_PENDING) == 0) {
                return true;
            }
            spin_hint();
        }
        return false;
    }

    static constexpr auto VECTOR_MASK = u32{0xFF};
    static constexpr auto INTERRUPT_MASK = u32{1} << 16;
    static constexpr auto TIMER_MODE_MASK = u32{3} << 17;

    /// SVR bit 8: the local APIC's software enable. Clear after reset.
    static constexpr auto SOFTWARE_ENABLE = u32{1} << 8;

    /// ICR bit 12, Delivery Status (read only): set while an IPI this local
    /// APIC issued has not yet been accepted by its destination.
    static constexpr auto ICR_DELIVERY_STATUS_PENDING = u32{1} << 12;

    /// How long try_wait_icr_idle() is willing to spin. A count of iterations
    /// and not a duration, deliberately: its callers are the paths that run
    /// before there is a clock to ask -- starting the very first AP among
    /// them. Each iteration is an uncached MMIO read plus a PAUSE, so this is
    /// a long time by any measure, which is what a give-up bound should be.
    static constexpr auto MAX_ICR_SPINS = usize{1'000'000};

    volatile u32 *mmio_base_{};
};
} // namespace arch

#endif // ARCH_X86_APIC_XAPIC_HPP
