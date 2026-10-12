/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_EXCEPTION_HPP
#define OURS_EXCEPTION_HPP 1

#include <ours/types.hpp>

#include <cstddef>

namespace ours {
/// What kind of exception a thread raised. The values are ABI and never
/// change: an exception channel reports them in ExceptionInfo::type. Bit 15
/// marks a synthetic exception, one the kernel raises for something other
/// than a CPU check.
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
    /// Synthetic: a process is starting. Raised by its first thread before
    /// that thread runs its first instruction, and sent only to the debugger
    /// channels of the process's job, one after another. Whatever state the
    /// handle is closed with, the process goes on starting: the debugger is
    /// told, it does not decide.
    ProcessStarting = 0x8308,
};

/// Which rule a PolicyError broke. The values are ABI, for the same reason as
/// ExceptionType's.
enum class PolicyCode : u32 {
    /// A system call trapped from outside the process's vDSO.
    BadSyscall = 18,
};

/// The data of the message an exception channel delivers for each exception,
/// which carries exactly one handle besides: the exception's own.
///
/// A record crosses the ABI as bytes, so its layout is pinned below rather
/// than left to the compiler.
struct ExceptionInfo {
    /// The koid of the faulting thread's process.
    KoId pid;

    /// The koid of the faulting thread.
    KoId tid;

    ExceptionType type;

    /// Zero. Explicit, so that no byte the kernel sends is left over from
    /// whatever its stack held before.
    u8 padding1[4];
};
static_assert(sizeof(ExceptionInfo) == 24);
static_assert(alignof(ExceptionInfo) == 8);
static_assert(offsetof(ExceptionInfo, pid) == 0);
static_assert(offsetof(ExceptionInfo, tid) == 8);
static_assert(offsetof(ExceptionInfo, type) == 16);
static_assert(offsetof(ExceptionInfo, padding1) == 20);

/// task_create_exception_channel's options.
enum class ExceptionChannelOptions : u32 {
    /// The task's own channel: a process's or a thread's.
    None = 0,
    /// One of a job's debugger channels, which hear ProcessStarting for every
    /// process that starts in the job. A job takes up to
    /// MAX_JOB_DEBUGGER_CHANNELS of them; a process does not take one yet
    /// (NotSupported), and a thread never does (InvArgs).
    Debugger = 1,
};

/// How many debugger channels one job takes at a time. One whose reader has
/// gone no longer counts.
inline constexpr u32 MAX_JOB_DEBUGGER_CHANNELS = 32;

/// What a handler decides about an exception, set on the exception's handle
/// as Property::ExceptionState before the handle is closed. Closing it is what
/// tells the kernel the handler is done; the state says what to do next.
///
/// The values are ABI. 2 is kept for a third state, in which the thread exits
/// instead of resuming, and comes with a consumer for it.
enum class ExceptionState : u32 {
    /// The default: the exception goes on to the next handler, and once none
    /// is left the process is killed with TASK_RETCODE_EXCEPTION_KILL.
    TryNext = 0,

    /// The thread resumes from where it was stopped, with whatever registers
    /// it has then. For a system call trapped from outside the vDSO, the call
    /// returns BadSyscall.
    Handled = 1,
};
} // namespace ours

#endif // #ifndef OURS_EXCEPTION_HPP
