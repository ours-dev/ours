/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_INTERRUPT_HPP
#define ARCH_X86_INTERRUPT_HPP 1

// Only ever reached via <arch/interrupt.hpp>'s own #include — don't include
// this file directly.

#include <arch/types.hpp>
#include <arch/x86/macro/irqframe.hpp>

#include <cstddef>

namespace arch {
enum class IrqVec : usize {
    DivideError = 0,
    Debug = 1,
    Nmi = 2,
    Breakpoint = 3,
    Overflow = 4,
    BoundRangeExceeded = 5,
    InvalidOpcode = 6,
    DeviceNotAvailable = 7,
    DoubleFault = 8,
    CoprocessorSegmentOverrun = 9,
    InvalidTss = 10,
    SegmentNotPresent = 11,
    StackFaultException = 12,
    GeneralProtection = 13,
    PageFault = 14,
    SpuriousInterrupt = 15,
    X87FloatingPoint = 16,
    AlignmentCheck = 17,
    MachineCheck = 18,
    SimdFloatingPoint = 19,
    Virtualizatoin = 20,
    ControlProtection = 21,
    VmmCommunicationException = 31,
    IretException = 31,
    PlatformIrqMin = 32,
    PlatformIrqMax = 0xEF,
    ApicSpurious = 0xF0,
    ApicTimer,
    ApicError,
    ApicPmi,
    IpiGeneric,
    IpiResched,
    IpiInterrupt,
    IpiSuspend,
    IpiTimer,
    LastUserDefined = 0xFF,
};

[[nodiscard]]
constexpr auto to_string(IrqVec vector) noexcept -> char const * {
    switch (vector) {
        case IrqVec::DivideError:
            return "divide error";
        case IrqVec::Debug:
            return "debug";
        case IrqVec::Nmi:
            return "nmi";
        case IrqVec::Breakpoint:
            return "breakpoint";
        case IrqVec::Overflow:
            return "overflow";
        case IrqVec::BoundRangeExceeded:
            return "bound range exceeded";
        case IrqVec::InvalidOpcode:
            return "invalid opcode";
        case IrqVec::DeviceNotAvailable:
            return "device not available";
        case IrqVec::DoubleFault:
            return "double fault";
        case IrqVec::InvalidTss:
            return "invalid tss";
        case IrqVec::SegmentNotPresent:
            return "segment not present";
        case IrqVec::StackFaultException:
            return "stack fault";
        case IrqVec::GeneralProtection:
            return "general protection";
        case IrqVec::PageFault:
            return "page fault";
        // Named too, because a user thread can raise them and the report that
        // kills its process prints this: "interrupt", the default below,
        // would misname a floating-point fault.
        case IrqVec::CoprocessorSegmentOverrun:
            return "coprocessor segment overrun";
        case IrqVec::X87FloatingPoint:
            return "x87 floating point";
        case IrqVec::AlignmentCheck:
            return "alignment check";
        case IrqVec::MachineCheck:
            return "machine check";
        case IrqVec::SimdFloatingPoint:
            return "simd floating point";
        case IrqVec::Virtualizatoin:
            return "virtualization";
        case IrqVec::ControlProtection:
            return "control protection";
        case IrqVec::ApicTimer:
            return "apic timer";
        case IrqVec::ApicError:
            return "apic error";
        case IrqVec::ApicPmi:
            return "apic pmi";
        case IrqVec::IpiGeneric:
            return "generic ipi";
        case IrqVec::IpiResched:
            return "reschedule ipi";
        case IrqVec::IpiInterrupt:
            return "interrupt ipi";
        case IrqVec::IpiSuspend:
            return "suspend ipi";
        case IrqVec::IpiTimer:
            return "timer ipi";
        default:
            return "interrupt";
    }
}

class ArchCpuState final {
  public:
    enum Field : usize {
        IF = 9,
        IOPL = 12,
        /// Alignment check / access control. With CR4.SMAP set it is the one
        /// bit that lets ring 0 touch user pages, which is why the page-fault
        /// handler reads it out of a frame (see x86_handle_page_fault()).
        AC = 18,
    };

    constexpr ArchCpuState() noexcept = default;

    [[nodiscard]]
    static auto read() noexcept -> ArchCpuState {
        usize value;
        asm volatile("pushfq; popq %0" : "=r"(value) : : "memory");
        return ArchCpuState(value);
    }

    auto write() const noexcept -> void {
        asm volatile("pushq %0; popfq" : : "r"(m_value) : "memory", "cc");
    }

    template <Field bit, typename T>
    constexpr auto set(T value) noexcept -> ArchCpuState & {
        if constexpr (bit == IOPL) {
            m_value = (m_value & ~(usize{3} << IOPL)) | ((static_cast<usize>(value) & 3) << IOPL);
        } else {
            m_value = static_cast<bool>(value) ? m_value | (usize{1} << bit)
                                               : m_value & ~(usize{1} << bit);
        }
        return *this;
    }

    template <Field bit>
    [[nodiscard]]
    constexpr auto get() const noexcept -> usize {
        if constexpr (bit == IOPL) {
            return (m_value >> IOPL) & 3;
        }
        return (m_value >> bit) & 1;
    }

  private:
    explicit constexpr ArchCpuState(usize value) noexcept : m_value(value) {}

    usize m_value{};
};

using IrqSavedState = ArchCpuState;

struct IrqFrame final {
    usize di;
    usize si;
    usize bp;
    usize bx;
    usize dx;
    usize cx;
    usize ax;
    usize r8;
    usize r9;
    usize r10;
    usize r11;
    usize r12;
    usize r13;
    usize r14;
    usize r15;
    IrqVec vector;
    usize error_code;
    union {
        usize ip;
        usize rip;
    };
    usize cs;
    ArchCpuState flags;
    usize usp;
    usize uss;

    /// The interrupted context in the terms arch-neutral code asks for, so it
    /// never has to know x86's register names or that the privilege level is
    /// the low two bits of CS. Each architecture's IrqFrame answers the same
    /// five questions; the sampling profiler and the lockup detector ask
    /// them.
    [[nodiscard]]
    auto pc() const -> usize {
        return ip;
    }

    /// The interrupted stack pointer. x86-64 pushes SS:RSP on every
    /// interrupt, from ring 0 as well as ring 3, so `usp` holds it either way.
    [[nodiscard]]
    auto sp() const -> usize {
        return usp;
    }

    [[nodiscard]]
    auto fp() const -> usize {
        return bp;
    }

    [[nodiscard]]
    auto from_user() const -> bool {
        return (cs & 0x3) != 0;
    }

    /// Whether the interrupted code ran with interrupts enabled: RFLAGS.IF
    /// as the CPU pushed it. A CPU stopped with it clear takes no tick and
    /// no IPI, only an NMI.
    [[nodiscard]]
    auto irqs_enabled() const -> bool {
        return flags.get<ArchCpuState::IF>() != 0;
    }
};

static_assert(offsetof(IrqFrame, di) == X86_IRQFRAME_OFFSET_DI);
static_assert(offsetof(IrqFrame, si) == X86_IRQFRAME_OFFSET_SI);
static_assert(offsetof(IrqFrame, vector) == X86_IRQFRAME_OFFSET_VECTOR);
static_assert(offsetof(IrqFrame, error_code) == X86_IRQFRAME_OFFSET_ERROR_CODE);
static_assert(offsetof(IrqFrame, ip) == X86_IRQFRAME_OFFSET_IP);
static_assert(offsetof(IrqFrame, cs) == X86_IRQFRAME_OFFSET_CS);
static_assert(offsetof(IrqFrame, flags) == X86_IRQFRAME_OFFSET_FLAGS);
static_assert(offsetof(IrqFrame, usp) == X86_IRQFRAME_OFFSET_USP);
static_assert(offsetof(IrqFrame, uss) == X86_IRQFRAME_OFFSET_USS);

inline auto local_irq_disable() noexcept -> void {
    asm volatile("cli" : : : "memory");
}

inline auto local_irq_enable() noexcept -> void {
    asm volatile("sti" : : : "memory");
}

[[nodiscard]]
inline auto irqs_disabled() noexcept -> bool {
    return ArchCpuState::read().get<ArchCpuState::IF>() == 0;
}

[[nodiscard]]
inline auto local_irq_save_state() noexcept -> IrqSavedState {
    auto const state = ArchCpuState::read();
    local_irq_disable();
    return state;
}

inline auto local_irq_restore_state(IrqSavedState state) noexcept -> void {
    state.write();
}
} // namespace arch

#endif // ARCH_X86_INTERRUPT_HPP
