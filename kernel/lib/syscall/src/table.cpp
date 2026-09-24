/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>
#include <syscall_abi/numbers.hpp>

#include "argument.hpp"

#include <array>
#include <iterator>

#include <arch/interrupt.hpp>
#include <arch/syscall.hpp>
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
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
        auto const *const process = object::ProcessDispatcher::current();
        if (process == nullptr) {
            return false;
        }
        return process->vdso_contains(frame->ip);
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
        // BadSyscall, not AccessDenied (design 4.5): AccessDenied is what a
        // handle lacking a right answers, and a program has to be able to tell
        // "your handle cannot do that" from "that was not a system call".
        // Zircon's sys_invalid_syscall() answers ZX_ERR_BAD_SYSCALL as well.
        frame->ax = static_cast<usize>(Status::BadSyscall);
    }

    // The dispatch layer: one shim per call, generated from
    // //system/public/ours/syscalls.gni, and GENERATED_SHIMS, the list of them
    // in call-number order. A shim's job is exactly two conversions and
    // nothing else -- each argument out of its register through
    // syscall::from_register() (argument.hpp), in the order
    // arch::syscall_arg() gives, and the result back into `ax` -- so there is
    // nothing in one that a person should have to write, or could get subtly
    // wrong by writing.
#include <syscall_abi/kernel_shims.inc>

    /// The answer to a system-call number that does not exist at all, as
    /// against one that exists and has no handler yet.
    ///
    /// It is an ordinary table entry rather than a branch target in the stub,
    /// which is what lets the range check be a `cmov` instead of a
    /// conditional jump: the stub clamps an out-of-range number onto this
    /// slot's index and calls through the table either way.
    ///
    /// BadSyscall rather than OutOfRange: OutOfRange is what a real call says
    /// about one of its *arguments*, and a caller has to be able to tell "you
    /// asked for a call that does not exist" from that. Zircon answers
    /// ZX_ERR_BAD_SYSCALL here.
    auto sys_bad_syscall_number(arch::SyscallFrame *const frame) -> void {
        frame->ax = static_cast<usize>(Status::BadSyscall);
    }

    /// The table x86_syscall indexes. %rax is user-controlled, so every slot
    /// has to be a valid target -- a null entry would be an indirect call
    /// through zero from user space.
    ///
    /// One entry longer than DISPATCH_SLOTS. The extra slot at the end is the
    /// out-of-range answer, and it exists so the range check can clamp rather
    /// than branch: a number at or above DISPATCH_SLOTS is rewritten to this
    /// index and dispatched like any other.
    ///
    /// Every call the description names is filled in from GENERATED_SHIMS;
    /// the numbers between the last of them and DISPATCH_SLOTS answer
    /// Unimplemented.
    ///
    /// 256 is the size the table had when the count lived in the public
    /// header as OURS_SYSCALL_COUNT. It is private now, and kept as it was
    /// only so that moving the numbers out changes nothing a caller can see;
    /// sizing the table by the generated count is a change of its own.
    constexpr auto DISPATCH_SLOTS = usize{256};
    static_assert(syscall_abi::SYSCALL_COUNT <= DISPATCH_SLOTS);

    constexpr auto TABLE_SIZE = DISPATCH_SLOTS + 1;

    constexpr auto make_syscall_table() {
        std::array<SyscallHandler, TABLE_SIZE> table{};
        for (auto &entry: table) {
            entry = &sys_not_implemented;
        }
        table[DISPATCH_SLOTS] = &sys_bad_syscall_number;

        // Index i of GENERATED_SHIMS is call number i: both are generated
        // from the position of the entry in syscalls.gni, which is the only
        // place a call's number is decided.
        for (auto number = usize(0); number < syscall_abi::SYSCALL_COUNT; ++number) {
            table[number] = GENERATED_SHIMS[number];
        }

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
    // The stub pushes an even number of slots onto a 16-byte aligned kernel
    // stack, so |frame| -- %rsp at its `call` -- is aligned the way the SysV
    // ABI promises every C++ function. Checked because every handler below
    // assumes it, and a misaligned stack says so only later, as an aligned
    // spill faulting somewhere unrelated.
    DEBUG_ASSERT(reinterpret_cast<usize>(frame) % 16 == 0, "misaligned syscall frame");

    // SYSCALL entered with IF clear (IA32_FMASK), and everything up to here
    // needed that: the stub was still juggling the user %rsp through a
    // per-CPU slot. It is in the frame now, on this thread's own stack, so
    // nothing below is tied to this CPU any more. Leaving IF clear past this
    // point would make every handler unpreemptible, carry a closed interrupt
    // window into every wait a handler enters, and cost this CPU its timer
    // ticks for as long as a long call ran. Zircon's do_syscall_pre()
    // re-enables at the same point, "to maintain kernel preemptiveness".
    arch::local_irq_enable();

    if (!trapped_from_vdso(frame)) [[unlikely]] {
        sys_not_from_vdso(frame);
    } else {
        // Clamped, not branched around: SYSCALL_TABLE has one entry more than
        // DISPATCH_SLOTS and that last slot answers Status::BadSyscall, so a
        // number at or above the count is rewritten onto it and dispatched
        // like any other. %rax is entirely user-controlled, and without this
        // the call below would index past the table by an offset the caller
        // chose.
        auto number = DISPATCH_SLOTS;
        if (frame->ax < DISPATCH_SLOTS) {
            number = frame->ax;
        }
        SYSCALL_TABLE[number](frame);
    }

    // Off again before the stub pops the frame. From its first pop onwards
    // the stub is restoring user state on the kernel stack, then loads the
    // user %rsp and swaps to the user GS: an interrupt taken in that window
    // would find a kernel CS with a user stack, or a user GS it would not
    // swap back. Zircon's do_syscall_post() disables here for the same
    // reason, and so that a pending-signal check (not written yet) cannot
    // lose a wakeup between the check and SYSRET.
    arch::local_irq_disable();
}

} // namespace ours
