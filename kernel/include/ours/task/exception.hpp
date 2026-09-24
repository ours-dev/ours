/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_EXCEPTION_HPP
#define OURS_TASK_EXCEPTION_HPP 1

#include <ours/types.hpp>

namespace arch {
struct IrqFrame;
} // namespace arch

namespace ours::task {
/// What kind of exception a user thread raised, as the kernel reports it.
///
/// Kernel-private for now: nothing outside the kernel sees one until there
/// are exception channels (Zircon's zx_task_create_exception_channel), and
/// the kind is only printed on the way to killing the process. The values
/// are Zircon's ZX_EXCP_* so that the day it becomes ABI it needs no
/// renumbering.
enum class ExceptionType : u32 {
    /// Any architectural fault not given a kind of its own below: #GP, #DE,
    /// #SS, #NP, #AC, the floating-point ones.
    General = 0x008,
    /// A page fault the address space could not resolve.
    FatalPageFault = 0x108,
    UndefinedInstruction = 0x208,
    SwBreakpoint = 0x308,
    HwBreakpoint = 0x408,
    UnalignedAccess = 0x508,
    /// Synthetic: the thread broke a rule rather than a CPU check -- a
    /// system call trapped from outside the vDSO, for one.
    PolicyError = 0x8208,
};

[[nodiscard]]
constexpr auto to_string(ExceptionType const type) -> char const * {
    switch (type) {
        case ExceptionType::General:
            return "General";
        case ExceptionType::FatalPageFault:
            return "FatalPageFault";
        case ExceptionType::UndefinedInstruction:
            return "UndefinedInstruction";
        case ExceptionType::SwBreakpoint:
            return "SwBreakpoint";
        case ExceptionType::HwBreakpoint:
            return "HwBreakpoint";
        case ExceptionType::UnalignedAccess:
            return "UnalignedAccess";
        case ExceptionType::PolicyError:
            return "PolicyError";
    }
    return "?";
}

/// Everything known about one user exception, gathered by the architecture
/// where it happened and carried up to the object layer, which decides what
/// becomes of the thread (today: its process is killed).
///
/// The architecture fills in what only it can read -- the vector's name, the
/// error code, CR2 -- at the point it can still read it, so that nothing above
/// has to include an arch header or re-read a register that may have changed
/// since.
///
/// Zircon counterpart: arch_exception_context_t, built by the x86 handlers and
/// handed to dispatch_user_exception().
struct ExceptionContext {
    ExceptionType type;
    /// The architecture's name for the vector ("page fault"), or the reason
    /// for a synthetic exception.
    char const *vector_name;
    usize error_code;
    VirtAddr ip;
    VirtAddr sp;
    /// The address that faulted, for a page fault; zero otherwise.
    VirtAddr fault_address;
    /// For a synthetic exception, which rule was broken and a detail of it
    /// (the call number, say); zero for an architectural one.
    u32 synth_code;
    u64 synth_data;
    /// The registers at the time, or null for a synthetic exception raised
    /// from inside a system call.
    arch::IrqFrame const *frame;
};

/// Prints the registers of |context|'s frame, or says there is none.
/// Implemented by the architecture (x86: faults.cpp).
auto arch_dump_exception_context(ExceptionContext const &context) -> void;

} // namespace ours::task

#endif // #ifndef OURS_TASK_EXCEPTION_HPP
