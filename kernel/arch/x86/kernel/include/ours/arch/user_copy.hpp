/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_USER_COPY_HPP
#define OURS_ARCH_USER_COPY_HPP 1

#include <ours/mm/vm_fault.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <ustl/option.hpp>

namespace ours {
/// The architecture's half of a user copy: move the bytes, and nothing else.
///
/// The range has been checked by the caller (//kernel/lib/user_copy) to lie
/// wholly in the user half; these do not look at it again. What they add is
/// the one thing the generic layer cannot express -- on a CPU with SMAP, ring
/// 0 may touch a user page only while RFLAGS.AC is set, so the copy is
/// bracketed by stac/clac and those two instructions bracket nothing else.
///
/// A copy that faults on a page the user's address space can supply (a
/// lazily committed one) is resolved by the page-fault handler like any other
/// and resumes. One that faults on a hole -- an unmapped page, a write to a
/// read-only mapping -- is sent back to the copy's own fault exit (the
/// data-fault resume, design 4.6), and these return InvArgs.
///
/// The copy is x86_user_copy or x86_user_copy_smap (user_copy.S), picked by
/// feature_has_smap. This kernel does not build with .code-patches, so the
/// choice is a cached flag rather than patched in at boot.
[[nodiscard]]
auto arch_copy_from_user(void *dst, VirtAddr src, usize size) -> Status;

[[nodiscard]]
auto arch_copy_to_user(VirtAddr dst, void const *src, usize size) -> Status;

/// A user-side fault a capturing copy did not resolve: where, and why --
/// exactly what Thread::Current::soft_fault() takes.
struct UserCopyFault {
    VirtAddr va;
    mm::VmfCause cause;
};

/// What a capturing copy returns. |fault| is set exactly when the copy stopped
/// on a user-side page fault; then |status| is InvArgs and the bytes before the
/// fault may or may not have been copied, so the caller has to redo the whole
/// request, not only what follows the fault.
///
/// It is part of the arch interface so that //kernel/lib/user_copy uses it as
/// it is.
struct UserCopyCaptureResult {
    Status status;
    ustl::Option<UserCopyFault> fault;
};

/// The copies above, except that a page fault on the user side is not resolved.
///
/// For a copy that has to run while its caller holds a lock. Resolving a fault
/// may block and takes the address space's lock, so doing it in place would
/// either sleep where sleeping is not allowed or take that lock inside the
/// caller's -- the wrong order, and a deadlock outright if the user buffer maps
/// the very object the caller has locked. Instead the fault comes back in the
/// result, and the caller drops its lock, calls soft_fault() with it, re-takes
/// the lock, looks its state up again, and retries.
///
/// These may run anywhere -- under a spin lock, with preemption or interrupts
/// off -- because a captured fault reaches neither the VM nor anything that
/// could block.
[[nodiscard]]
auto arch_copy_from_user_capture_faults(void *dst, VirtAddr src, usize size)
    -> UserCopyCaptureResult;

[[nodiscard]]
auto arch_copy_to_user_capture_faults(VirtAddr dst, void const *src, usize size)
    -> UserCopyCaptureResult;

} // namespace ours

#endif // #ifndef OURS_ARCH_USER_COPY_HPP
