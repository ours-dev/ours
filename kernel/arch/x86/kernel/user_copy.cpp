/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/arch/thread.hpp>
#include <ours/arch/user_copy.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/assert.hpp>

#include <arch/interrupt.hpp>

// The copy itself, in user_copy.S: one routine per SMAP setting, so that the
// stac/clac window is written out and holds the copy and nothing else. Each
// returns 0 when it copied everything and 1 when a user-side fault could not
// be resolved; see the file's header for how the fault gets back here.
extern "C" auto x86_user_copy(void *dst, void const *src, usize len, ours::VirtAddr *resume) -> u32;
extern "C" auto x86_user_copy_smap(void *dst, void const *src, usize len, ours::VirtAddr *resume)
    -> u32;

namespace ours {
namespace {
    /// The variant this CPU can run -- the plain one where stac/clac would be
    /// #UD, the bracketed one where SMAP would otherwise refuse the access --
    /// handed the running thread's resume slot.
    auto copy(void *const dst, void const *const src, usize const size) -> Status {
        auto *const resume = task::X86Thread::current_data_fault_resume();
        // Set means a copy is already in progress on this thread and this one
        // was entered from inside it -- from a fault handler, say. The inner
        // copy would clear the slot on its way out and leave the outer one
        // with no recovery at all.
        DEBUG_ASSERT(*resume == 0, "a user copy inside a user copy");

        // The range check that admitted this copy (user_copy::is_user_range)
        // is a branch, and the copy below runs on the predicted side of it
        // before it resolves. Mispredicted, a copy *from* a kernel address
        // reads kernel memory into a buffer the program will see the cache
        // trace of (Spectre V1), and a copy *to* one speculatively stores
        // over kernel data -- a return address, a function pointer -- that
        // other speculative code may then use (Spectre V1.1). lfence holds
        // everything after it until every earlier instruction, the check
        // included, has completed. Zircon's _arch_copy_to_or_from_user() puts
        // the same lfence between can_access() and the copy.
        asm volatile("lfence" ::: "memory");

        auto const faulted = feature_has_smap ? x86_user_copy_smap(dst, src, size, resume)
                                              : x86_user_copy(dst, src, size, resume);

        // Both exits of the copy close the window; the fault exit is the one
        // that gets no other exercise. AC left set would quietly turn SMAP off
        // for everything this thread does next. Zircon asserts the same after
        // _x86_copy_to_or_from_user().
        DEBUG_ASSERT(!feature_has_smap ||
                         arch::ArchCpuState::read().get<arch::ArchCpuState::AC>() == 0,
                     "a user copy returned with the SMAP window open");
        DEBUG_ASSERT(*resume == 0, "a user copy returned with its resume slot set");

        // InvArgs, as in Zircon: from the caller's side a pointer that turned
        // out not to be backed is a bad argument like any other, and the
        // system call returns it.
        return faulted ? Status::InvArgs : Status::Ok;
    }
} // namespace

auto arch_copy_from_user(void *const dst, VirtAddr const src, usize const size) -> Status {
    return copy(dst, reinterpret_cast<void const *>(src), size);
}

auto arch_copy_to_user(VirtAddr const dst, void const *const src, usize const size) -> Status {
    return copy(reinterpret_cast<void *>(dst), src, size);
}

} // namespace ours
