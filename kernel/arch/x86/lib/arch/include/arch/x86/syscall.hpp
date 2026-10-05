/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_SYSCALL_HPP
#define ARCH_X86_SYSCALL_HPP 1

// Only ever reached via <arch/syscall.hpp>'s own #include — don't include
// this file directly.

#include <arch/interrupt.hpp>
#include <arch/types.hpp>
#include <arch/x86/macro/syscallframe.hpp>

#include <cstddef>

namespace arch {
/// The user register state kernel/arch/x86/kernel/syscall.S saves before it
/// calls into the dispatch table, and restores from afterwards.
///
/// This is not an IrqFrame with fewer fields, and the difference is the
/// SYSCALL instruction rather than a choice: SYSCALL pushes nothing at all.
/// It puts the return address in %rcx and RFLAGS in %r11 -- destroying
/// whatever the caller had there -- and leaves %rsp pointing at the *user*
/// stack.  So there is no CS/SS pair in this frame (SYSRET reloads both from
/// IA32_STAR), no error code and no vector; and `ip`/`flags` come out of
/// %rcx/%r11 rather than off the stack.  The two slots those registers would
/// have occupied are pushed as literal zero, because their user values are
/// already gone by the time the first instruction of the stub runs and
/// storing the trashed values would invite a handler to believe them.
///
/// The user stack pointer is the last member, and it has to be in here rather
/// than anywhere per-CPU. SYSCALL leaves %rsp on the user stack, so the stub
/// can only stash it in a per-CPU scratch slot long enough to load the kernel
/// stack -- and it used to leave it there for the whole call, restoring it on
/// the way out. That holds exactly as long as no handler ever sleeps: the
/// first one that blocks lets another thread's syscall on this CPU overwrite
/// the slot, and a thread that wakes on a different CPU restores that CPU's
/// value instead. Either way SYSRET returns onto somebody else's stack. So the
/// stub copies the slot into the frame immediately, and the slot is scratch
/// for two instructions and nothing more.
///
/// The entry stub in kernel/arch/x86/kernel/syscall.S builds this frame.
///
/// A handler answers by writing `ax`; the stub pops the whole frame back
/// into the registers, so a return value passed the C++ way would be
/// overwritten on the way out.
struct SyscallFrame final {
    usize ax;
    usize bx;

    /// Always zero on entry.  SYSCALL overwrote %rcx with the user return
    /// address before the kernel got control; `ip` below is that value.
    usize cx;

    usize dx;
    usize si;
    usize di;
    usize bp;
    usize r8;
    usize r9;
    usize r10;

    /// Always zero on entry, for the same reason as `cx`: SYSCALL overwrote
    /// %r11 with the user RFLAGS, which is what `flags` below holds.
    usize r11;

    usize r12;
    usize r13;
    usize r14;
    usize r15;

    /// The user instruction to resume at -- SYSCALL's saved %rcx, which
    /// SYSRET copies back into %rip.
    usize ip;

    /// The user RFLAGS -- SYSCALL's saved %r11, masked by IA32_FMASK on the
    /// way in and restored by SYSRET on the way out.
    ArchCpuState flags;

    /// The user %rsp, popped straight back into %rsp on the way out.
    ///
    /// It is also what makes the frame an even number of slots: eighteen
    /// pushes from a 16-byte aligned stack top leave %rsp aligned at the
    /// `call`, which the SysV ABI requires and seventeen did not provide.
    usize sp;
};

static_assert(offsetof(SyscallFrame, ax) == X86_SYSCALLFRAME_OFFSET_AX);
static_assert(offsetof(SyscallFrame, bx) == X86_SYSCALLFRAME_OFFSET_BX);
static_assert(offsetof(SyscallFrame, cx) == X86_SYSCALLFRAME_OFFSET_CX);
static_assert(offsetof(SyscallFrame, dx) == X86_SYSCALLFRAME_OFFSET_DX);
static_assert(offsetof(SyscallFrame, si) == X86_SYSCALLFRAME_OFFSET_SI);
static_assert(offsetof(SyscallFrame, di) == X86_SYSCALLFRAME_OFFSET_DI);
static_assert(offsetof(SyscallFrame, bp) == X86_SYSCALLFRAME_OFFSET_BP);
static_assert(offsetof(SyscallFrame, r8) == X86_SYSCALLFRAME_OFFSET_R8);
static_assert(offsetof(SyscallFrame, r9) == X86_SYSCALLFRAME_OFFSET_R9);
static_assert(offsetof(SyscallFrame, r10) == X86_SYSCALLFRAME_OFFSET_R10);
static_assert(offsetof(SyscallFrame, r11) == X86_SYSCALLFRAME_OFFSET_R11);
static_assert(offsetof(SyscallFrame, r12) == X86_SYSCALLFRAME_OFFSET_R12);
static_assert(offsetof(SyscallFrame, r13) == X86_SYSCALLFRAME_OFFSET_R13);
static_assert(offsetof(SyscallFrame, r14) == X86_SYSCALLFRAME_OFFSET_R14);
static_assert(offsetof(SyscallFrame, r15) == X86_SYSCALLFRAME_OFFSET_R15);
static_assert(offsetof(SyscallFrame, ip) == X86_SYSCALLFRAME_OFFSET_IP);
static_assert(offsetof(SyscallFrame, flags) == X86_SYSCALLFRAME_OFFSET_FLAGS);
static_assert(offsetof(SyscallFrame, sp) == X86_SYSCALLFRAME_OFFSET_SP);
static_assert(sizeof(SyscallFrame) % 16 == 0, "the stub calls C++ with this frame on top");
static_assert(sizeof(SyscallFrame) == X86_SYSCALLFRAME_SIZE);

/// The length of the SYSCALL instruction (0f 05). The frame records the
/// address after it, so this is how far back the instruction itself starts.
constexpr auto SYSCALL_INSN_SIZE = usize{2};

/// The register the |N|th argument of a system call (counting from zero)
/// arrived in: %rdi, %rsi, %rdx, %r10, %r8, %r9, %r12, %r13.
///
/// The first six are the SysV argument registers with one substitution, and
/// the substitution is forced. SYSCALL overwrites %rcx with the user return
/// address and %r11 with RFLAGS before the kernel gets control -- that is why
/// their slots in the frame are pushed as zero -- so neither can ever carry an
/// argument, and the vDSO stub moves the fourth argument, which SysV passes in
/// %rcx, to %r10. SysV puts the seventh and eighth on the caller's stack; the
/// stub loads them into %r12 and %r13, which the frame saves like every other
/// register. A ninth is refused by the generator rather than read from the
/// user stack, which would take a user_copy on every call that had one.
template <usize N>
[[nodiscard]]
constexpr auto syscall_arg(SyscallFrame const *const frame) -> usize {
    static_assert(N < 8, "a system call takes at most eight arguments");
    if constexpr (N == 0) {
        return frame->di;
    } else if constexpr (N == 1) {
        return frame->si;
    } else if constexpr (N == 2) {
        return frame->dx;
    } else if constexpr (N == 3) {
        return frame->r10;
    } else if constexpr (N == 4) {
        return frame->r8;
    } else if constexpr (N == 5) {
        return frame->r9;
    } else if constexpr (N == 6) {
        return frame->r12;
    } else {
        return frame->r13;
    }
}

} // namespace arch

#endif // ARCH_X86_SYSCALL_HPP
