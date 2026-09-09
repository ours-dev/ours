/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VMALLOC_ARENA_HPP
#define OURS_MM_VMALLOC_ARENA_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::mm {
/// The address half of vmalloc: which pages of the vmalloc window are free.
///
/// Deliberately knows nothing about physical memory, page tables, locks or
/// the VMM. It hands out and takes back page-aligned ranges of a window whose
/// address it is told once, and that is all -- which is what lets it be tested
/// on the build host, where none of the rest exists. mm::vmalloc() supplies
/// everything else.
///
/// Linux counterpart: the free-VA half of mm/vmalloc.c -- `struct vmap_area`
/// in the address-sorted rb-tree augmented with each subtree's largest free
/// extent, allocated through `alloc_vmap_area()` and returned by
/// `merge_or_add_vmap_area()`. The rb-tree exists there to make first-fit
/// cheap across the hundreds of thousands of ranges a running Linux system
/// holds; this kernel has a handful, so a sorted array of holes is both faster
/// (no pointer chasing, no rebalancing) and far easier to be sure about.
///
/// Free ranges rather than busy ones, because the two are not symmetric: a
/// free hole is nothing but an extent, while a busy range also owns the
/// physical pages behind it and has to be found from an interior pointer.
/// mm::vmalloc() keeps the busy side.
class VmallocArena {
    using Self = VmallocArena;

  public:
    /// How many disjoint free holes the window may be broken into.
    ///
    /// The bound this needs to satisfy is `holes <= live allocations + 1`:
    /// allocating carves from the front of a hole, so it leaves the count
    /// unchanged or one lower, and only a free that touches neither neighbour
    /// can raise it. Refusing to allocate past `MAX_HOLES - 1` live ranges
    /// therefore keeps the count inside the array without free() ever having
    /// to fail -- and a free that fails would be strictly worse than an
    /// allocation that fails, because it would leak address space with no way
    /// to report it.
    ///
    /// 256 is far above what this kernel does (the handle arena, userboot's
    /// package buffer, the profiler's unwind table); it is the size at which
    /// the array is still one page.
    static constexpr auto MAX_HOLES = usize{256};

    /// Claims |base| to |base + size| as the window. Both must be page
    /// aligned and the size non-zero.
    auto init(VirtAddr base, usize size) -> Status;

    /// Carves |nr_pages| contiguous pages out of the window.
    ///
    /// Returns 0, not an address, when there is no room: zero is never a
    /// legal answer here (the window is in the kernel half), and every caller
    /// already has to test the result of the physical allocation that follows,
    /// so a Status would only be unpacked and thrown away.
    ///
    /// Lowest-address first fit, as Linux's `find_vmap_lowest_match()` is.
    /// Best-fit would pack the window tighter, and packing is not the problem
    /// a window this empty has; first fit keeps freed ranges near the bottom
    /// where the next allocation will merge with them.
    [[nodiscard]]
    auto allocate(usize nr_pages) -> VirtAddr;

    /// Returns a range this arena handed out. |base| and |nr_pages| must be
    /// exactly what allocate() answered and was asked for: the arena keeps no
    /// record of busy ranges, so it cannot check a partial return, and a
    /// caller that returns the wrong extent corrupts the window silently.
    /// DEBUG builds check what they can (in range, page aligned, not already
    /// free).
    auto free(VirtAddr base, usize nr_pages) -> Status;

    [[nodiscard]]
    auto base() const -> VirtAddr {
        return m_base;
    }

    [[nodiscard]]
    auto nr_holes() const -> usize {
        return m_nr_holes;
    }

    [[nodiscard]]
    auto nr_live() const -> usize {
        return m_nr_live;
    }

    /// Pages not currently handed out. The boot self-check compares this
    /// against the window size to prove a sequence of allocations and frees
    /// left nothing behind.
    [[nodiscard]]
    auto nr_free_pages() const -> usize;

    /// Asserts the invariants: holes sorted by address, none empty, none
    /// adjacent (adjacent ones must have been merged), all inside the window.
    /// A no-op unless DEBUG_ASSERT is compiled in.
    auto validate() const -> void;

  private:
    struct Hole {
        VirtAddr base;
        usize nr_pages;
    };

    /// Where |base| belongs in the sorted array: the index of the first hole
    /// at or above it.
    [[nodiscard]]
    auto lower_bound(VirtAddr base) const -> usize;

    auto insert_hole_at(usize index, VirtAddr base, usize nr_pages) -> void;
    auto erase_hole_at(usize index) -> void;

    Hole m_holes[MAX_HOLES]{};
    usize m_nr_holes{};
    usize m_nr_live{};
    VirtAddr m_base{};
    usize m_nr_pages{};
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VMALLOC_ARENA_HPP
