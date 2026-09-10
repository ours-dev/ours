/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <array>

#include <arch/syscall.hpp>
#include <logz4/log.hpp>
#include <ours/compiler.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/task/thread.hpp>
#include <ours/types.hpp>

namespace ours {
namespace {
    /// x86_syscall (arch/x86/kernel/syscall.S) saves the user registers into
    /// an arch::SyscallFrame on the kernel stack and then does
    /// `call *SYSCALL_TABLE(, %rax, 8)` with %rdi holding a pointer to it. It
    /// pops the whole frame back into the registers afterwards, so a handler
    /// answers by writing `frame->ax` rather than by returning a value -- a
    /// C++ return value would be overwritten on the way out.
    ///
    /// The frame used to be spelled `void *` here, because nothing in the tree
    /// described the stub's push order. arch::SyscallFrame does now
    /// (arch/x86/syscall.hpp), and it is static_asserted against the same
    /// offset macros the stub is written from.
    using SyscallHandler = auto (*)(arch::SyscallFrame *frame) -> void;

    auto sys_not_implemented(arch::SyscallFrame *const frame) -> void {
        frame->ax = static_cast<usize>(Status::Unimplemented);
    }

    /// Whether this trap came from the calling process's vDSO.
    ///
    /// The rule this enforces is that a user program may enter the kernel only
    /// through the vDSO. `frame->ip` is the user return address -- the value
    /// SYSCALL parked in %rcx -- so it is the address of the instruction
    /// *after* the trap, and testing it says where the trap came from.
    ///
    /// It is the address after, not of, the `syscall` instruction, so the
    /// range has to cover one byte past the last instruction of the vDSO.
    /// It does: the mapping is page-granular and the image is padded to a page
    /// boundary, so there is always something after the final instruction.
    ///
    /// A thread with no user thread is a kernel thread, and a kernel thread
    /// does not execute `syscall` -- it calls the functions directly. Refusing
    /// is the safe direction for a caller this cannot identify.
    ///
    /// Zircon counterpart: VDso::ValidSyscallPC, consulted on the same path in
    /// do_syscall(). Zircon's is finer: it checks against the exact set of
    /// approved return addresses (the CODE_SYSRET_* labels), so even a jump
    /// into the middle of the vDSO cannot forge a call site. The vDSO here has
    /// the same labels waiting (OURS_VDSO_SYSRET_*); what is missing is a way
    /// to get their addresses to the kernel, which is a build-time question.
    /// A range check is the honest subset: every trap from outside the vDSO is
    /// refused, which is the property that was asked for.
    [[nodiscard]]
    auto trapped_from_vdso(arch::SyscallFrame const *const frame) -> bool {
        auto const *const thread = task::Thread::Current::get();
        if (thread == nullptr) {
            return false;
        }
        auto const &user_thread = thread->user_thread();
        if (!user_thread) {
            return false;
        }
        return user_thread->process().vdso_contains(frame->ip);
    }

    /// The answer to a trap that did not come through the vDSO.
    ///
    /// Refused rather than killed, for now. Zircon raises a policy exception,
    /// which is the right end state -- a program that forged a trap is not one
    /// whose next instruction should be trusted -- but there is no exception
    /// machinery here yet, and a status the caller cannot ignore is already
    /// the difference between a rule and a wish.
    auto sys_not_from_vdso(arch::SyscallFrame *const frame) -> void {
        log::warn("syscall: refused a trap from {:X}, which is outside the vDSO", frame->ip);
        frame->ax = static_cast<usize>(Status::AccessDenied);
    }

    /// The dispatch layer, one shim per call.
    ///
    /// This is the piece table.cpp's own TODO used to say was missing. Its job
    /// is exactly two conversions and nothing else: the arguments come out of
    /// the frame in the order the x86-64 SysV registers put them, and the
    /// Status goes back into `ax`, because the stub pops the frame into the
    /// registers on the way out and a C++ return value would be discarded.
    ///
    /// %rcx and %r11 are not readable here and never will be: the SYSCALL
    /// instruction overwrote both before the kernel got control, which is why
    /// their slots in the frame are pushed as zero and why a fourth argument
    /// would have to come from %r10.
    auto sys_debug_write_shim(arch::SyscallFrame *const frame) -> void {
        auto const status = sys_debug_write(reinterpret_cast<char const *>(frame->di), frame->si);
        frame->ax = static_cast<usize>(status);
    }

    /// The answer to a system-call number that does not exist at all, as
    /// against one that exists and has no handler yet.
    ///
    /// It is an ordinary table entry rather than a branch target in the stub,
    /// which is what lets the range check be a `cmov` instead of a
    /// conditional jump: the stub clamps an out-of-range number onto this
    /// slot's index and calls through the table either way.
    auto sys_bad_syscall_number(arch::SyscallFrame *const frame) -> void {
        frame->ax = static_cast<usize>(Status::OutOfRange);
    }

    /// The table x86_syscall indexes. %rax is user-controlled, so every slot
    /// has to be a valid target -- a null entry would be an indirect call
    /// through zero from user space.
    ///
    /// One entry longer than SYSCALL_COUNT. The extra slot at the end is the
    /// out-of-range answer, and it exists so the stub's range check can clamp
    /// rather than branch: a number at or above SYSCALL_COUNT is rewritten to
    /// this index and dispatched like any other. Without it the stub would
    /// have to jump around the call and set the return value in assembly,
    /// which means spelling a Status constant there.
    ///
    /// TODO(SmallHuaZi): only OURS_SYS_DEBUG_WRITE is filled in. The rest
    /// answer Unimplemented, because lib/syscall's other typed entry points
    /// (sys_thread_spawn and friends) take object::Handle*, and resolving a
    /// user's handle number into one needs the handle table that does not
    /// exist yet.
    constexpr auto TABLE_SIZE = SYSCALL_COUNT + 1;

    constexpr auto make_syscall_table() {
        std::array<SyscallHandler, TABLE_SIZE> table{};
        for (auto &entry: table) {
            entry = &sys_not_implemented;
        }
        table[SYSCALL_COUNT] = &sys_bad_syscall_number;

        table[OURS_SYS_DEBUG_WRITE] = &sys_debug_write_shim;

        return table;
    }
} // namespace

extern "C" constinit auto SYSCALL_TABLE = make_syscall_table();

/// The one door, and the only place the vDSO rule is checked.
///
/// x86_syscall calls this rather than indexing SYSCALL_TABLE itself. Putting
/// the check here rather than in each shim is what makes it impossible to
/// forget: a new syscall is a new table entry, and a table entry cannot be
/// reached except through this function.
///
/// The number is clamped here too, which used to be a `cmov` in the stub. Two
/// mechanisms for one property read worse than one, and the assembly could not
/// have done the vDSO half anyway -- it cannot reach the current thread, whose
/// pointer lives behind a mangled C++ per-CPU template.
extern "C" auto syscall_dispatch(arch::SyscallFrame *const frame) -> void {
    if (!trapped_from_vdso(frame)) [[unlikely]] {
        return sys_not_from_vdso(frame);
    }

    // Clamped, not branched around: SYSCALL_TABLE has one entry more than
    // SYSCALL_COUNT and that last slot answers Status::OutOfRange, so a number
    // at or above the count is rewritten onto it and dispatched like any
    // other. %rax is entirely user-controlled, and without this the call below
    // would index past the table by an offset the caller chose.
    auto const number = frame->ax < SYSCALL_COUNT ? frame->ax : SYSCALL_COUNT;
    SYSCALL_TABLE[number](frame);
}

} // namespace ours
