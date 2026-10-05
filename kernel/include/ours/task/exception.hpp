/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_EXCEPTION_HPP
#define OURS_TASK_EXCEPTION_HPP 1

#include <ours/exception.hpp>
#include <ours/types.hpp>

namespace arch {
struct IrqFrame;
} // namespace arch

namespace ours::task {
/// The kinds of exception and policy codes are ABI since exception channels
/// report them (<ours/exception.hpp>); the kernel uses the same definitions
/// rather than a copy that could drift.
using ours::ExceptionType;
using ours::PolicyCode;

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

[[nodiscard]]
constexpr auto to_string(PolicyCode const code) -> char const * {
    switch (code) {
        case PolicyCode::BadSyscall:
            return "BadSyscall";
    }
    return "?";
}

/// What becomes of a thread that raised a user exception, as the object layer
/// decides it; the architecture acts on nothing else. Resume returns to ring 3
/// through the frame the exception saved, which a handler may have changed.
/// End never returns there: the object layer has given the thread the Kill
/// signal, its process's or its own, and the pending-signal check on the way
/// out ends it.
enum class ExceptionOutcome : u8 {
    Resume,
    End,
};

/// Everything known about one user exception, gathered by the architecture
/// where it happened and carried up to the object layer, which decides what
/// becomes of the thread (today: its process is killed).
///
/// The architecture fills in what only it can read -- the vector's name, the
/// error code, CR2 -- at the point it can still read it, so that nothing above
/// has to include an arch header or re-read a register that may have changed
/// since.
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
