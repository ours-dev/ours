/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/vmalloc_arena.hpp>

#include <limits>

namespace ours::mm {
namespace {
    /// The one piece of arithmetic this file does, named so the multiplication
    /// cannot be written the wrong way round at a call site.
    constexpr auto pages_to_bytes(usize const nr_pages) -> usize {
        return nr_pages * PAGE_SIZE;
    }
} // namespace

auto VmallocArena::init(VirtAddr const base, usize const size) -> Status {
    if (base == 0 || size == 0) {
        return Status::InvArgs;
    }
    if (base % PAGE_SIZE != 0 || size % PAGE_SIZE != 0) {
        return Status::InvArgs;
    }
    // The window must not wrap: allocate() and free() both compare
    // `base + pages_to_bytes(n)` against the end, and a wrapped end would make
    // every such comparison answer the opposite of the truth.
    if (size > std::numeric_limits<VirtAddr>::max() - base) {
        return Status::InvArgs;
    }

    m_base = base;
    m_nr_pages = size / PAGE_SIZE;
    m_nr_holes = 1;
    m_nr_live = 0;
    m_holes[0] = Hole{.base = base, .nr_pages = m_nr_pages};
    return Status::Ok;
}

auto VmallocArena::nr_free_pages() const -> usize {
    auto total = usize{0};
    for (auto index = usize{0}; index < m_nr_holes; ++index) {
        total += m_holes[index].nr_pages;
    }
    return total;
}

auto VmallocArena::lower_bound(VirtAddr const base) const -> usize {
    // Linear rather than binary: MAX_HOLES is 256 and the array is one cache-
    // friendly run, so the branch-free scan wins over a search that mispredicts
    // every step. If the bound ever grows by an order of magnitude this is the
    // line to revisit.
    auto index = usize{0};
    while (index < m_nr_holes && m_holes[index].base < base) {
        ++index;
    }
    return index;
}

auto VmallocArena::insert_hole_at(usize const index, VirtAddr const base, usize const nr_pages)
    -> void {
    DEBUG_ASSERT(m_nr_holes < MAX_HOLES, "vmalloc arena hole array overflow");
    DEBUG_ASSERT(index <= m_nr_holes);

    // Backwards, so the source of each move is a slot that has already been
    // copied out of.
    for (auto slot = m_nr_holes; slot > index; --slot) {
        m_holes[slot] = m_holes[slot - 1];
    }
    m_holes[index] = Hole{.base = base, .nr_pages = nr_pages};
    ++m_nr_holes;
}

auto VmallocArena::erase_hole_at(usize const index) -> void {
    DEBUG_ASSERT(index < m_nr_holes);

    for (auto slot = index; slot + 1 < m_nr_holes; ++slot) {
        m_holes[slot] = m_holes[slot + 1];
    }
    --m_nr_holes;
}

auto VmallocArena::allocate(usize const nr_pages) -> VirtAddr {
    if (nr_pages == 0 || nr_pages > m_nr_pages) {
        return 0;
    }

    // The guard that keeps free() infallible. Every live range can, when it is
    // returned, split a hole in two and so add one; capping the live count at
    // MAX_HOLES - 1 bounds the array by MAX_HOLES no matter what order the
    // returns come in. See the note on MAX_HOLES.
    if (m_nr_live + 1 >= MAX_HOLES) {
        return 0;
    }

    for (auto index = usize{0}; index < m_nr_holes; ++index) {
        auto &hole = m_holes[index];
        if (hole.nr_pages < nr_pages) {
            continue;
        }

        auto const base = hole.base;
        if (hole.nr_pages == nr_pages) {
            erase_hole_at(index);
        } else {
            // Carved from the front, which is what keeps the array sorted
            // without moving anything: the hole's base rises, and it was
            // already above its predecessor.
            hole.base += pages_to_bytes(nr_pages);
            hole.nr_pages -= nr_pages;
        }

        ++m_nr_live;
        validate();
        return base;
    }

    return 0;
}

auto VmallocArena::free(VirtAddr const base, usize const nr_pages) -> Status {
    if (nr_pages == 0 || base % PAGE_SIZE != 0) {
        return Status::InvArgs;
    }

    auto const window_end = m_base + pages_to_bytes(m_nr_pages);
    auto const end = base + pages_to_bytes(nr_pages);
    if (base < m_base || end > window_end || end < base) {
        return Status::InvArgs;
    }

    auto const index = lower_bound(base);

    // Overlap with a neighbouring hole means the range was never handed out,
    // or was handed back twice. Nothing above this layer can tell the two
    // apart, and neither can this one -- but either is a bug in the caller,
    // and continuing would merge a live range into the free list and hand it
    // out again while its owner still holds it.
    if (index < m_nr_holes) {
        DEBUG_ASSERT(end <= m_holes[index].base, "vmalloc free overlaps a free hole");
    }
    if (index > 0) {
        // Read only by the assert below, which is not compiled in a release
        // build.
        [[maybe_unused]]
        auto const &previous = m_holes[index - 1];
        DEBUG_ASSERT(previous.base + pages_to_bytes(previous.nr_pages) <= base,
                     "vmalloc free overlaps a free hole");
    }

    auto const merges_left =
        index > 0 && m_holes[index - 1].base + pages_to_bytes(m_holes[index - 1].nr_pages) == base;
    auto const merges_right = index < m_nr_holes && end == m_holes[index].base;

    if (merges_left && merges_right) {
        // The range filled the gap between two holes, so the three become one
        // and the count falls.
        m_holes[index - 1].nr_pages += nr_pages + m_holes[index].nr_pages;
        erase_hole_at(index);
    } else if (merges_left) {
        m_holes[index - 1].nr_pages += nr_pages;
    } else if (merges_right) {
        m_holes[index].base = base;
        m_holes[index].nr_pages += nr_pages;
    } else {
        insert_hole_at(index, base, nr_pages);
    }

    DEBUG_ASSERT(m_nr_live > 0, "vmalloc free with no live range");
    --m_nr_live;
    validate();
    return Status::Ok;
}

auto VmallocArena::validate() const -> void {
    DEBUG_ASSERT(m_nr_holes <= MAX_HOLES);

    // Every use of this is inside a DEBUG_ASSERT, so a release build sees it
    // written and never read.
    [[maybe_unused]]
    auto previous_end = m_base;
    for (auto index = usize{0}; index < m_nr_holes; ++index) {
        auto const &hole = m_holes[index];
        DEBUG_ASSERT(hole.nr_pages > 0, "empty hole in the vmalloc arena");
        DEBUG_ASSERT(hole.base >= previous_end, "vmalloc arena holes out of order or overlapping");
        // Strictly greater for every hole after the first: two holes that meet
        // should have been merged by free(), and leaving them apart would let
        // an allocation fail with room available on both sides of a seam.
        if (index > 0) {
            DEBUG_ASSERT(hole.base > previous_end, "adjacent vmalloc arena holes were not merged");
        }
        previous_end = hole.base + pages_to_bytes(hole.nr_pages);
        DEBUG_ASSERT(previous_end <= m_base + pages_to_bytes(m_nr_pages),
                     "vmalloc arena hole runs past the window");
    }
}

} // namespace ours::mm
