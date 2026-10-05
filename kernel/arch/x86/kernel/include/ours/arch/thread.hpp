/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_THREAD_HPP
#define OURS_ARCH_THREAD_HPP 1

#include <ours/compiler.hpp>
#include <ours/task/types.hpp>
#include <ours/types.hpp>

#include <gktl/canary.hpp>
#include <ustl/mem/container_of.hpp>

namespace ours::task {
class Thread;

class X86Thread {
    using Self = X86Thread;

  public:
    /// Switches the CPU from `prev` to `next`.
    ///
    /// Takes `Thread *`, not `Self *`, because that is the type it works in:
    /// the very first thing it needs is `next`'s kernel stack top, which
    /// lives on Thread, and it used to recover that with `Thread::of(next)`
    /// -- a container_of back up from a parameter the caller had just
    /// narrowed down. The narrowing bought nothing: Thread declares
    /// `friend ArchThread`, so the arch half has always been able to reach
    /// its enclosing Thread, and a signature that hides that only makes the
    /// dependency harder to see.
    static auto switch_thread(Thread *prev, Thread *next) -> void;

    auto init(VirtAddr entry_point) -> void;

    /// What the running thread's user copy in progress has told the page-fault
    /// handler (design 4.6). All zero outside a copy.
    struct UserCopyState {
        /// Bit 63 of |resume|: whether a fault inside the copy is first handed
        /// to the page-fault handler to resolve.
        ///
        /// Set, the fault is resolved in place like any other and the copy goes
        /// on; only a fault that cannot be resolved is sent to the copy's exit.
        /// Clear, the fault is not resolved at all. The handler sends it
        /// straight to the exit with the faulting address in %rdx and its
        /// VmfCause in %rcx, and the copy returns them to its caller. That is
        /// fault capture: a copy that runs under a lock -- where resolving would
        /// take the address space's lock inside it, or block where blocking is
        /// not allowed -- gets the fault back, drops the lock, resolves it with
        /// soft_fault(), and retries.
        ///
        /// The flag can live inside the address because every kernel address
        /// has bit 63 set (the kernel's half starts at KERNEL_ASPACE_BASE and
        /// runs to the top), so the exit's address is recovered by setting it
        /// again. user_copy.cpp asserts that at compile time.
        static constexpr VirtAddr RUN_FAULT_HANDLER = VirtAddr(1) << 63;

        /// The address of the copy's own fault exit, with RUN_FAULT_HANDLER
        /// cleared when the copy captures its faults. x86_user_copy
        /// (user_copy.S) stores it before touching user memory and clears it
        /// before returning.
        VirtAddr resume = 0;

        /// The user operand of the copy -- the range the range check admitted.
        /// A fault inside the copy is resumed only if it lies here. The copy
        /// has two operands and only one of them is the user's; a fault on the
        /// other one (a null kernel buffer, say) is a kernel bug that should
        /// stop the kernel, not an InvArgs the system call quietly returns.
        VirtAddr user_base = 0;
        usize user_size = 0;
    };

    /// The running thread's user-copy state.
    ///
    /// Per thread, not per CPU: a copy that faults on a lazily committed page
    /// may block while the page is supplied, and wake on another CPU, and the
    /// state has to go with it.
    static auto current_user_copy() -> UserCopyState *;

  private:
    friend class Thread;

    VirtAddr m_fs_base;
    VirtAddr m_gs_base;
    VirtAddr m_sp;
    UserCopyState m_user_copy;
};
using ArchThread = X86Thread;

} // namespace ours::task

#endif // #ifndef OURS_ARCH_THREAD_HPP