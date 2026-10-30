/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/arch/aspace_layout.hpp>
#include <ours/arch/thread.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/arch_user_copy.hpp>
#include <ours/assert.hpp>

#include <cstddef>

#include <arch/interrupt.hpp>

// What the copy in user_copy.S returns, in %rax:%rdx: whether it took its fault
// exit, and for a capturing copy that did, the fault's VmfCause and address.
// The layout is the ABI between the assembly and this file -- faulted in the
// low half of %rax, the cause in the high half, the address in %rdx -- so it is
// pinned down here rather than left to whatever the compiler would pick.
struct X86UserCopyRet {
    u32 faulted;
    u32 cause;
    ours::VirtAddr va;
};
static_assert(sizeof(X86UserCopyRet) == 16 && offsetof(X86UserCopyRet, cause) == 4 &&
                  offsetof(X86UserCopyRet, va) == 8,
              "x86_user_copy returns this struct in %rax:%rdx; see user_copy.S");

// The copy itself, in user_copy.S: one routine per SMAP setting, so that the
// stac/clac window is written out and holds the copy and nothing else. See the
// file's header for how a fault gets back here, and for what |resume_mask| does.
extern "C" auto x86_user_copy(void *dst, void const *src, usize len, ours::VirtAddr *resume,
                              ours::VirtAddr resume_mask) -> X86UserCopyRet;
extern "C" auto x86_user_copy_smap(void *dst, void const *src, usize len, ours::VirtAddr *resume,
                                   ours::VirtAddr resume_mask) -> X86UserCopyRet;

namespace ours {
namespace {
    using UserCopyState = task::X86Thread::UserCopyState;

    /// The two resume masks. Resolving leaves the exit's address untouched;
    /// capturing clears RUN_FAULT_HANDLER in it, which the page-fault handler
    /// reads as "hand this fault back, do not resolve it".
    constexpr VirtAddr RESOLVE_FAULTS = ~VirtAddr(0);
    constexpr VirtAddr CAPTURE_FAULTS = ~UserCopyState::RUN_FAULT_HANDLER;

    // The flag rides in the exit's address, and the handler recovers the
    // address by setting the bit again. That is only lossless if every address
    // the exit can have -- every kernel address -- already has it set. The
    // kernel's half starts at KERNEL_ASPACE_BASE and runs to the top of the
    // address space, so checking the base is enough.
    static_assert((KERNEL_ASPACE_BASE & UserCopyState::RUN_FAULT_HANDLER) != 0,
                  "RUN_FAULT_HANDLER must be set in every kernel address");

    /// The variant this CPU can run -- the plain one where stac/clac would be
    /// #UD, the bracketed one where SMAP would otherwise refuse the access --
    /// with the running thread's user-copy state filled in for the fault
    /// handler: the resume slot the routine writes, and |user|, the operand
    /// the range check admitted, which is the only range a fault may be
    /// resumed from.
    auto copy(void *const dst, void const *const src, usize const size, VirtAddr const user,
              VirtAddr const resume_mask) -> X86UserCopyRet {
        auto &state = *task::X86Thread::current_user_copy();
        // Set means a copy is already in progress on this thread and this one
        // was entered from inside it -- from a fault handler, say. The inner
        // copy would clear the state on its way out and leave the outer one
        // with no recovery at all.
        DEBUG_ASSERT(state.resume == 0, "a user copy inside a user copy");

        state.user_base = user;
        state.user_size = size;

        // The range check that admitted this copy (user_copy::is_user_range)
        // is a branch, and the copy below runs on the predicted side of it
        // before it resolves. Mispredicted, a copy *from* a kernel address
        // reads kernel memory into a buffer the program will see the cache
        // trace of (Spectre V1), and a copy *to* one speculatively stores
        // over kernel data -- a return address, a function pointer -- that
        // other speculative code may then use (Spectre V1.1). lfence holds
        // everything after it until every earlier instruction, the check
        // included, has completed.
        asm volatile("lfence" ::: "memory");

        auto const result = FEATURE_HAS_SMAP
                                ? x86_user_copy_smap(dst, src, size, &state.resume, resume_mask)
                                : x86_user_copy(dst, src, size, &state.resume, resume_mask);

        state.user_base = 0;
        state.user_size = 0;

        // Both exits of the copy close the window; the fault exit is the one
        // that gets no other exercise. AC left set would quietly turn SMAP off
        // for everything this thread does next.
        DEBUG_ASSERT(!FEATURE_HAS_SMAP ||
                         arch::ArchCpuState::read().get<arch::ArchCpuState::AC>() == 0,
                     "a user copy returned with the SMAP window open");
        DEBUG_ASSERT(state.resume == 0, "a user copy returned with its resume slot set");

        return result;
    }

    /// InvArgs: from the caller's side a pointer that turned out not to be
    /// backed is a bad argument like any other, and the system call returns it.
    auto to_status(X86UserCopyRet const &result) -> Status {
        if (result.faulted != 0) {
            return Status::InvArgs;
        }
        return Status::Ok;
    }

    /// The fault goes back to the caller only when there was one. A capturing
    /// copy that finished has nothing to report, and %rcx:%rdx then hold
    /// whatever the copy left in them.
    auto to_capture_result(X86UserCopyRet const &result) -> UserCopyCaptureResult {
        if (result.faulted == 0) {
            return UserCopyCaptureResult{.status = Status::Ok, .fault = ustl::NONE};
        }
        return UserCopyCaptureResult{
            .status = Status::InvArgs,
            .fault = UserCopyFault{.va = result.va, .cause = mm::VmfCause(result.cause)},
        };
    }
} // namespace

// On a CPU with SMAP, ring 0 may touch a user page only while RFLAGS.AC is
// set, so the copy is bracketed by stac/clac and those two instructions
// bracket nothing else. The copy is x86_user_copy or x86_user_copy_smap
// (user_copy.S), picked by FEATURE_HAS_SMAP. This kernel does not build with
// .code-patches, so the choice is a cached flag rather than patched in at
// boot.
auto arch_copy_from_user(void *const dst, VirtAddr const src, usize const size) -> Status {
    return to_status(copy(dst, reinterpret_cast<void const *>(src), size, src, RESOLVE_FAULTS));
}

auto arch_copy_to_user(VirtAddr const dst, void const *const src, usize const size) -> Status {
    return to_status(copy(reinterpret_cast<void *>(dst), src, size, dst, RESOLVE_FAULTS));
}

auto arch_copy_from_user_capture_faults(void *const dst, VirtAddr const src, usize const size)
    -> UserCopyCaptureResult {
    return to_capture_result(
        copy(dst, reinterpret_cast<void const *>(src), size, src, CAPTURE_FAULTS));
}

auto arch_copy_to_user_capture_faults(VirtAddr const dst, void const *const src, usize const size)
    -> UserCopyCaptureResult {
    return to_capture_result(copy(reinterpret_cast<void *>(dst), src, size, dst, CAPTURE_FAULTS));
}

} // namespace ours
