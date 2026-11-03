/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USER_COPY_RANGE_HPP
#define USER_COPY_RANGE_HPP 1

#include <ours/types.hpp>

namespace user_copy {
/// Whether [addr, addr + size) lies wholly inside [base, base + span).
///
/// Written without ever computing `addr + size`, because that is the sum a
/// caller controls and it can wrap: a range that starts in user space and
/// wraps past the top of the address space lands its end back below the
/// split, and a check of the form `addr >= base && addr + size <= limit`
/// passes it. Working in offsets from |base| leaves nothing to overflow.
///
/// A zero-sized range still has to *start* inside: a pointer argument is a
/// claim that the address is a user address, and a length of zero does not
/// make a kernel address acceptable.
///
/// Pure arithmetic with no dependency on the address-space layout, so that the
/// edge cases can be pinned down at compile time (below) and on the host
/// (test/user_copy_test.cpp) rather than only by booting.
[[nodiscard]]
constexpr auto is_within(ours::VirtAddr const addr, usize const size, ours::VirtAddr const base,
                         usize const span) -> bool {
    if (addr < base) {
        return false;
    }
    auto const offset = addr - base;
    if (offset >= span) {
        return false;
    }
    return size <= span - offset;
}

// The cases that have gone wrong in hand-written range checks elsewhere in
// this tree, checked on every build.
static_assert(is_within(ours::VirtAddr{0x2000}, 0x1000, ours::VirtAddr{0x1000}, 0x2000),
              "ends exactly at the limit");
static_assert(!is_within(ours::VirtAddr{0x2000}, 0x1001, ours::VirtAddr{0x1000}, 0x2000),
              "one byte past the limit");
static_assert(!is_within(ours::VirtAddr{0xfff}, 1, ours::VirtAddr{0x1000}, 0x2000),
              "starts below the base");
static_assert(!is_within(ours::VirtAddr{0x3000}, 0, ours::VirtAddr{0x1000}, 0x2000),
              "empty, but starts at the limit");
static_assert(is_within(ours::VirtAddr{0x1000}, 0, ours::VirtAddr{0x1000}, 0x2000),
              "empty and inside");
static_assert(!is_within(ours::VirtAddr{0x1800}, ~usize{0}, ours::VirtAddr{0x1000}, 0x2000),
              "a size that would wrap the sum");

} // namespace user_copy

#endif // #ifndef USER_COPY_RANGE_HPP
