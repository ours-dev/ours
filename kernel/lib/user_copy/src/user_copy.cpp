/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <user_copy/range.hpp>
#include <user_copy/user_copy.hpp>

#include <ours/arch/aspace_layout.hpp>
#include <ours/arch/user_copy.hpp>
#include <ours/mm/cfg.hpp>

namespace user_copy {
auto is_user_range(ours::VirtAddr const addr, usize const size) -> bool {
    // The user half as the address-space layout defines it, not "anything
    // below the kernel". The two differ by the null hole under
    // USER_ASPACE_BASE and by the non-canonical gap above the 2^47 split, and
    // an access into the gap is a #GP rather than a page fault -- a crash no
    // range check further down would have caught.
    return is_within(addr, size, USER_ASPACE_BASE, USER_ASPACE_SIZE);
}

auto copy_from_user(void *const dst, ours::VirtAddr const src, usize const size) -> ours::Status {
    if (!is_user_range(src, size)) {
        return ours::Status::InvArgs;
    }
    if (size == 0) {
        return ours::Status::Ok;
    }
    return ours::arch_copy_from_user(dst, src, size);
}

auto copy_to_user(ours::VirtAddr const dst, void const *const src, usize const size)
    -> ours::Status {
    if (!is_user_range(dst, size)) {
        return ours::Status::InvArgs;
    }
    if (size == 0) {
        return ours::Status::Ok;
    }
    return ours::arch_copy_to_user(dst, src, size);
}

} // namespace user_copy
