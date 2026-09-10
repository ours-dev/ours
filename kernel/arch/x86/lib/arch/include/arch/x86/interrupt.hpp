/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_INTERRUPT_HPP
#define ARCH_X86_INTERRUPT_HPP 1

// Only ever reached via <arch/interrupt.hpp>'s own #include — don't include
// this file directly.

#include <cstddef>

#include <arch/types.hpp>
#include <arch/x86/macro/irqframe.hpp>

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
        default:
            return "interrupt";
    }
}

class ArchCpuState final {
  public:
    enum Field : usize {
        IF = 9,
        IOPL = 12,
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
