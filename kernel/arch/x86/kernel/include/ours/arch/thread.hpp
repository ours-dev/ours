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

    /// The running thread's data-fault resume slot (design 4.6).
    ///
    /// Zero, except while the thread is inside x86_user_copy (user_copy.S),
    /// which stores the address of its own fault exit here before it touches
    /// user memory and clears it before returning. A page fault on a user
    /// address that the ordinary handler cannot resolve, taken with this set
    /// and at an instruction inside the copy, resumes there instead of
    /// panicking, and the copy returns failure.
    ///
    /// Per thread, not per CPU: a copy that faults on a lazily committed page
    /// may block while the page is supplied, and wake on another CPU, and the
    /// slot has to go with it.
    ///
    /// Zircon counterpart: arch().page_fault_resume, read by x86_pfe_handler().
    static auto current_data_fault_resume() -> VirtAddr *;

  private:
    friend class Thread;

    VirtAddr m_fs_base;
    VirtAddr m_gs_base;
    VirtAddr m_sp;
    VirtAddr m_data_fault_resume = 0;
};
using ArchThread = X86Thread;

} // namespace ours::task

#endif // #ifndef OURS_ARCH_THREAD_HPP