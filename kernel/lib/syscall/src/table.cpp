/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <array>
#include <iterator>

#include "argument.hpp"
#include <arch/interrupt.hpp>
#include <arch/nospec.hpp>
#include <arch/syscall.hpp>
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <syscall_abi/numbers.hpp>

namespace ours {
namespace {
    /// x86_syscall (arch/x86/kernel/syscall.S) saves the user registers into
    /// an arch::SyscallFrame on the kernel stack and calls syscall_dispatch()
    /// below with %rdi pointing at it; the dispatcher checks where the trap
    /// came from, clamps and confines the number, and calls the handler out
    /// of SYSCALL_TABLE. The stub pops the whole frame back into the
    /// registers afterwards, so a handler answers by writing `frame->ax`
    /// rather than by returning a value -- a C++ return value would be
    /// overwritten on the way out.
    ///
    /// The frame used to be spelled `void *` here, because nothing in the tree
    /// described the stub's push order. arch::SyscallFrame does now
    /// (arch/x86/syscall.hpp), and it is static_asserted against the same
    /// offset macros the stub is written from.
    using SyscallHandler = auto (*)(arch::SyscallFrame *frame) -> void;

    /// Whether this trap came from the calling process's vDSO.
    ///
    /// The rule this enforces is that a user program may enter the kernel only
    /// through the vDSO. `frame->ip` is the user return address -- the value
    /// SYSCALL parked in %rcx -- so it is the address of the instruction
    /// *after* the trap, and testing it says where the trap came from.
    ///
    /// It is the address after, not of, the `syscall` instruction, so what is
    /// checked is the instruction itself: the SYSCALL_INSN_SIZE bytes that end
    /// at `frame->ip`, all of which have to be the vDSO's. Checking the return
    /// address alone admitted a `syscall` sitting in the last two bytes
    /// *below* the vDSO, whose return address is the vDSO's first byte -- and
    /// the program image is mapped below it. Nothing could reach that while no
    /// system call maps memory, but vmar_map would have.
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
        return process->vdso_contains(frame->ip - arch::SYSCALL_INSN_SIZE, arch::SYSCALL_INSN_SIZE);
    }

    /// The answer to a trap that did not come through the vDSO: the process
    /// is killed.
    ///
    /// A program that forged a trap is not one whose next instruction should
    /// be trusted, so returning an error it could ignore is not enough. It is
    /// raised as a PolicyError exception (BadSyscall, the call number as the
    /// detail), whose handling today is to kill the process; the thread exits
    /// at the pending-signal check on the way back out
    /// (x86_syscall_process_pending_signals), before SYSRET.
    ///
    /// Zircon counterpart: sys_invalid_syscall(), which raises
    /// ZX_EXCP_POLICY_CODE_BAD_SYSCALL through SignalPolicyException().
    auto not_from_vdso(arch::SyscallFrame *const frame) -> void {
        log::warn("syscall: refused a trap from {:#x}, which is outside the vDSO", frame->ip);

        // Only a user thread executes `syscall`; a kernel thread calls the
        // functions directly. One getting here is a kernel bug, and there is
        // no process to kill for it.
        auto *const thread = object::ThreadDispatcher::current();
        ASSERT(thread, "a trap from a kernel thread at {:#x}", frame->ip);
        thread->on_bad_syscall(frame->ax, frame->ip, frame->sp);

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

    /// The answer to a system-call number that names no call.
    ///
    /// It is an ordinary table entry rather than a branch target, which is
    /// what lets the range check clamp instead of jump: syscall_dispatch()
    /// rewrites an out-of-range number onto this slot's index and calls
    /// through the table either way.
    ///
    /// BadSyscall rather than OutOfRange: OutOfRange is what a real call says
    /// about one of its *arguments*, and a caller has to be able to tell "you
    /// asked for a call that does not exist" from that. Zircon answers
    /// ZX_ERR_BAD_SYSCALL here.
    auto bad_syscall_number(arch::SyscallFrame *const frame) -> void {
        frame->ax = static_cast<usize>(Status::BadSyscall);
    }

    /// The table syscall_dispatch() indexes, with the number from %rax
    /// clamped and confined. %rax is user-controlled, so every slot
    /// has to be a valid target -- a null entry would be an indirect call
    /// through zero from user space.
    ///
    /// One entry longer than the number of calls. The extra slot at the end
    /// is the out-of-range answer, and it exists so the range check can clamp
    /// rather than branch: a number at or above syscall_abi::SYSCALL_COUNT is
    /// rewritten to this index and dispatched like any other.
    ///
    /// Sized by the generated count, so that every slot below the sentinel is
    /// a call the description names. It used to be 256 slots, fixed in the
    /// public header, with the unnamed ones answering Unimplemented -- "this
    /// call exists and has no handler yet" -- for numbers that were never
    /// calls at all. With the numbers private to the vDSO and the kernel,
    /// nothing reserves a range of them any more, and a number that names no
    /// call is BadSyscall whatever its value, as in Zircon's unknown_syscall.
    constexpr auto TABLE_SIZE = syscall_abi::SYSCALL_COUNT + 1;

    constexpr auto make_syscall_table() {
        std::array<SyscallHandler, TABLE_SIZE> table{};
        table[syscall_abi::SYSCALL_COUNT] = &bad_syscall_number;

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

// Every slot a valid target. The table is no longer pre-filled with a default
// answer, so a slot the loops above miss would stay null, and %rax is the
// user's: that would be an indirect call through zero on request.
static_assert(
    [] {
        for (auto const entry: make_syscall_table()) {
            if (entry == nullptr) {
                return false;
            }
        }
        return true;
    }(),
    "every SYSCALL_TABLE slot must have a handler");

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
        not_from_vdso(frame);
    } else {
        // Clamped, not branched around: SYSCALL_TABLE has one entry more than
        // there are calls, and that last slot answers Status::BadSyscall, so a
        // number at or above the count is rewritten onto it and dispatched
        // like any other. %rax is entirely user-controlled, and without this
        // the call below would index past the table by an offset the caller
        // chose.
        auto number = syscall_abi::SYSCALL_COUNT;
        if (frame->ax < syscall_abi::SYSCALL_COUNT) {
            number = frame->ax;
        }
        // And confined, because the clamp above is C++ and may well compile
        // to a branch. Mispredicted, it would hand the load below the raw
        // %rax, and the CPU would fetch a "handler" from wherever the caller
        // pointed past the table and speculatively call it -- a jump to an
        // address of the user's choosing, however briefly. confine_array_index
        // turns any index past the table into 0 with a conditional move that
        // cannot run ahead of its comparison, so the worst a wrong path can do
        // is start the call in slot 0. Architecturally it changes nothing:
        // |number| is already in range by the time it gets here. Zircon's
        // x86 syscall.S pairs its `jae .Lunknown_syscall` with a `cmovge` of
        // zero for the same reason.
        number = arch::confine_array_index(number, TABLE_SIZE);
        SYSCALL_TABLE[number](frame);
    }

    // Off again before the stub pops the frame. From its first pop onwards
    // the stub is restoring user state on the kernel stack, then loads the
    // user %rsp and swaps to the user GS: an interrupt taken in that window
    // would find a kernel CS with a user stack, or a user GS it would not
    // swap back. Zircon's do_syscall_post() disables here for the same
    // reason, and so that the pending-signal check the stub makes next
    // (x86_syscall_process_pending_signals) cannot lose a wakeup between the
    // check and SYSRET.
    arch::local_irq_disable();
}

} // namespace ours
