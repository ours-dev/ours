/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_USER_COPY_HPP
#define OURS_ARCH_USER_COPY_HPP 1

#include <ours/types.hpp>

namespace ours {
/// The architecture's half of a user copy: move the bytes, and nothing else.
///
/// The range has been checked by the caller (//kernel/lib/user_copy) to lie
/// wholly in the user half; these do not look at it again. What they add is
/// the one thing the generic layer cannot express -- on a CPU with SMAP, ring
/// 0 may touch a user page only while RFLAGS.AC is set, so the copy is
/// bracketed by stac/clac and those two instructions bracket nothing else.
///
/// There is no fault recovery yet. A copy that faults on a page the user's
/// address space can supply (a lazily committed one) is resolved by the page
/// fault handler like any other and resumes; one that faults on a hole is a
/// kernel panic. That is the stated P0 limit -- only programs from the kernel
/// package run -- and Zircon's data_fault_resume is the planned answer
/// (docs/design/260923/userspace-interface.md, 4.6).
///
/// Zircon counterpart: arch_copy_from_user()/arch_copy_to_user() in
/// kernel/arch/x86/user_copy.cc, which pick a stac/clac or plain variant by
/// code patching. This kernel does not build with .code-patches, so the choice
/// is a cached flag instead (feature_has_smap).
auto arch_copy_from_user(void *dst, VirtAddr src, usize size) -> void;

auto arch_copy_to_user(VirtAddr dst, void const *src, usize size) -> void;

} // namespace ours

#endif // #ifndef OURS_ARCH_USER_COPY_HPP
