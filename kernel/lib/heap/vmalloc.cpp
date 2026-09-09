/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/sync/mutex.hpp>

#include <heap/vmalloc.hpp>
#include <heap/vmalloc_arena.hpp>

namespace heap {
using ours::Guard;
using ours::Status;
using ours::mm::MmuFlags;

namespace {
    /// Read/write and nothing else. A vmalloc range holds data -- buffers,
    /// tables, arenas -- so leaving it executable would hand every one of them
    /// to an attacker looking for a place to put code, and no caller has ever
    /// wanted otherwise. A future vmalloc_exec(), if the kernel ever loads
    /// modules, would be a separate entry point with its own justification.
    constexpr auto MMU_FLAGS = MmuFlags::Readable | MmuFlags::Writable;

    /// One record per live range, so that vfree() can recover the length from
    /// a bare pointer.
    ///
    /// The physical frames are deliberately *not* recorded. They are already
    /// written down in the page tables, and reading them back with query() at
    /// teardown costs one walk per page on a path that runs rarely -- against
    /// storing a frame list per range, which would need somewhere to put it
    /// and would be a second copy of the truth to keep in step with the first.
    /// Linux keeps `struct vm_struct::pages` because its unmap is batched and
    /// deferred, so the page tables are gone before the pages are returned;
    /// the teardown below is synchronous and can still read them.
    struct BusyRange {
        ours::VirtAddr base;
        usize nr_pages;
    };

    /// No allocation, and the bound is the arena's own: it refuses to hand out
    /// more than MAX_HOLES - 1 live ranges, so a table of that size can never
    /// be the thing that fills up first. An allocator that has to allocate to
    /// record what it allocated is a recursion waiting to be discovered.
    VmallocArena ARENA;
    BusyRange BUSY[VmallocArena::MAX_HOLES];
    usize NR_BUSY;
    bool READY;

    /// Guards all three of the above and the page-table edits between them.
    ///
    /// A Mutex rather than a SpinLock, because everything under it can block:
    /// mm::alloc_frames() takes the current thread's resource lock and may
    /// walk into reclaim, and the page-table walk allocates table pages. That
    /// is what makes "vmalloc may not be called from an interrupt handler" a
    /// property of the design rather than a warning.
    ours::sync::Mutex LOCK;

    /// Tears down |nr_pages| pages of mapping starting at |base|, returning
    /// every frame behind them.
    ///
    /// The order inside the loop is the whole point, and it is the opposite of
    /// Linux's: read the frame, drop the translation, *then* return the frame.
    /// Linux frees first and lets __purge_vmap_area_lazy() batch the flush,
    /// which is safe there only because the lazy purge holds the address range
    /// back until it has flushed. Nothing here holds anything back, so a frame
    /// returned before its translation was invalidated could be handed to
    /// someone else while a stale TLB entry still reaches it.
    ///
    /// Tolerates pages that are not mapped, which is what lets the failure
    /// path of vmalloc() call it on a half-built range.
    auto unmap_and_free(ours::mm::VmAspace &aspace, ours::VirtAddr const base, usize const nr_pages)
        -> void {
        for (auto index = usize{0}; index < nr_pages; ++index) {
            auto const va = base + index * PAGE_SIZE;

            ours::PhysAddr pa = 0;
            MmuFlags flags{};
            if (aspace.arch_aspace().query(va, &pa, &flags) != Status::Ok) {
                continue;
            }

            usize unmapped = 0;
            auto const status =
                aspace.arch_aspace().unmap(va, 1, arch::paging::UnmapControl::None, &unmapped);
            if (status != Status::Ok) {
                // The frame is deliberately kept rather than returned: a page
                // whose translation is still live is not free memory, and
                // leaking it is the lesser of the two outcomes.
                log::error("vmalloc: could not unmap {:X}: {}", va, ours::to_string(status));
                continue;
            }

            ours::mm::free_frames(ours::mm::phys_to_frame(pa), 0);
        }
    }
} // namespace

auto init_vmalloc() -> void {
    Guard guard{LOCK};

    auto const status = ARENA.init(VMALLOC_BASE, VMALLOC_SIZE);
    ASSERT(Status::Ok == status, "Failed to initialise the vmalloc window");

    NR_BUSY = 0;
    READY = true;
    log::info("vmalloc: window [{:X}, {:X}), {} pages", VMALLOC_BASE, VMALLOC_BASE + VMALLOC_SIZE,
              VMALLOC_SIZE / PAGE_SIZE);
}

auto vmalloc(usize const size, Gaf const gaf) -> void * {
    if (size == 0) {
        return nullptr;
    }

    auto const nr_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    // The round-up above wraps for a size within a page of the top of the
    // address space, and a wrapped count would ask the arena for nothing at
    // all and then be mapped as if it were huge.
    if (nr_pages == 0 || nr_pages > VMALLOC_SIZE / PAGE_SIZE) {
        return nullptr;
    }

    Guard guard{LOCK};

    // Reachable, and worth reporting rather than asserting: `operator new`
    // routes here by size, and the kernel allocates before init_vmm() runs
    // (SparseMemMap builds its arrays at InitLevel::Pmm). A caller that asks
    // too early gets the same nullptr it would get from an exhausted window,
    // which is what its failure path already handles.
    if (!READY) {
        log::error("vmalloc: called before the window was initialised");
        return nullptr;
    }
    if (NR_BUSY == std::size(BUSY)) {
        return nullptr;
    }

    // One page more than asked for, left unmapped: a guard, so that running
    // off the end of a vmalloc range faults instead of walking into whatever
    // the next allocation put there. Linux does the same by default and calls
    // the exception VM_NO_GUARD. Note it is counted in the *address*
    // reservation only -- everything below maps and frees nr_pages -- which
    // is the distinction Linux draws between vm_struct::size and
    // get_vm_area_size().
    auto const base = ARENA.allocate(nr_pages + 1);
    if (base == 0) {
        return nullptr;
    }

    ours::mm::PmFrame::List frames;
    auto status = ours::mm::alloc_frames(NUMA_MAX_NODE, gaf, &frames, nr_pages);
    if (Status::Ok != status) {
        static_cast<void>(ARENA.free(base, nr_pages + 1));
        return nullptr;
    }

    auto aspace = ours::mm::VmAspace::kernel_aspace();

    // One page at a time, taken off the list as it is consumed. Per page
    // rather than through map_bulk() because the frames arrive as a list and
    // batching them would need an array of physical addresses to put them in
    // -- which is the storage this design is built to avoid. The cost is a
    // page-table walk per page on a path that runs a handful of times.
    //
    // The frames must be drained rather than left on the list: the hooks are
    // auto-unlink, so a node still linked when its list head dies points at a
    // dead head.
    auto mapped = usize{0};
    while (!frames.empty()) {
        auto &frame = frames.front().value();
        frames.pop_front();

        usize done = 0;
        status =
            aspace->arch_aspace().map(base + mapped * PAGE_SIZE, ours::mm::frame_to_phys(&frame), 1,
                                      MMU_FLAGS, arch::paging::MapControl::ErrorIfExisting, &done);
        if (Status::Ok != status) {
            // This frame is off the list and not yet mapped, so it is returned
            // here; the ones still on the list go back together; and the pages
            // already installed are torn down by the same routine vfree() uses.
            ours::mm::free_frames(&frame, 0);
            ours::mm::free_frames(&frames);
            unmap_and_free(*aspace, base, mapped);
            static_cast<void>(ARENA.free(base, nr_pages + 1));
            log::error("vmalloc: could not map page {} of {}: {}", mapped, nr_pages,
                       ours::to_string(status));
            return nullptr;
        }
        ++mapped;
    }
    DEBUG_ASSERT(mapped == nr_pages,
                 "vmalloc mapped a different number of pages than it asked for");

    BUSY[NR_BUSY++] = BusyRange{.base = base, .nr_pages = nr_pages};
    return reinterpret_cast<void *>(base);
}

auto vfree(void *const pointer) -> void {
    if (pointer == nullptr) {
        return;
    }

    auto const base = ours::VirtAddr(pointer);
    DEBUG_ASSERT(is_vmalloc_addr(pointer), "vfree() on an address outside the vmalloc window");

    Guard guard{LOCK};

    auto found = std::size(BUSY);
    for (auto index = usize{0}; index < NR_BUSY; ++index) {
        if (BUSY[index].base == base) {
            found = index;
            break;
        }
    }
    if (found == std::size(BUSY)) {
        // An interior pointer, a double free, or a pointer this allocator
        // never produced. None of them can be served, and guessing at a length
        // would unmap memory belonging to someone else.
        DEBUG_ASSERT(false, "vfree() on an address this allocator did not hand out");
        return;
    }

    auto const nr_pages = BUSY[found].nr_pages;
    // Swap with the last live record rather than shifting: the table has no
    // ordering to preserve, since it is searched linearly.
    BUSY[found] = BUSY[--NR_BUSY];

    auto aspace = ours::mm::VmAspace::kernel_aspace();
    unmap_and_free(*aspace, base, nr_pages);
    static_cast<void>(ARENA.free(base, nr_pages + 1));
}

auto vmalloc_free_pages() -> usize {
    Guard guard{LOCK};
    return ARENA.nr_free_pages();
}

/// Proves the allocator works on the machine it was built for, once, at boot.
///
/// It exists because the host test beside it cannot: the arena's arithmetic is
/// pure and testable off a kernel, but nothing else here is -- the frames, the
/// page tables, the guard page and the teardown order only mean anything with
/// an MMU underneath. This is the smallest thing that exercises all of them.
///
/// The check that matters most is the last one. Every other case would fail
/// loudly; a leak would not, and a leak is exactly what the teardown order and
/// the guard-page arithmetic are easiest to get wrong in. Comparing the free
/// page count before and against after catches a range whose guard page was
/// mapped, or whose frames were dropped, or whose address was never returned.
auto vmalloc_self_test() -> void {
    auto const before = vmalloc_free_pages();
    auto failed = usize{0};

    auto const check = [&failed](bool const ok, char const *const what) {
        if (!ok) {
            log::error("vmalloc: FAILED {}", what);
            ++failed;
        }
    };

    // A single page, written end to end: if the mapping were short or the
    // permissions wrong this faults rather than returning.
    auto *one = static_cast<u8 *>(vmalloc(PAGE_SIZE, ours::mm::GAF_KERNEL));
    check(one != nullptr, "one-page allocation");
    if (one) {
        check(is_vmalloc_addr(one), "the result is inside the window");
        check(ours::VirtAddr(one) % PAGE_SIZE == 0, "the result is page aligned");
        for (auto index = usize{0}; index < PAGE_SIZE; ++index) {
            one[index] = u8(index);
        }
        check(one[0] == 0 && one[PAGE_SIZE - 1] == u8(PAGE_SIZE - 1),
              "the page holds what was written");
    }

    // A size that is neither a page multiple nor a power of two: the round-up
    // is where an off-by-one hides, and get_frames_n() -- the hand-rolled path
    // this replaces -- would have quietly rounded 9 KiB up to 16.
    auto *odd = static_cast<u8 *>(vmalloc(2 * PAGE_SIZE + 1, ours::mm::GAF_KERNEL));
    check(odd != nullptr, "odd-sized allocation");
    if (odd) {
        odd[0] = 1;
        odd[2 * PAGE_SIZE] = 2;
        check(odd[0] == 1 && odd[2 * PAGE_SIZE] == 2, "the last page of an odd size is mapped");
    }

    // Two live ranges must not overlap, and the guard page must keep them a
    // page apart even when the arena hands out adjacent addresses.
    if (one && odd) {
        auto const gap = ours::VirtAddr(odd) - (ours::VirtAddr(one) + PAGE_SIZE);
        check(gap >= PAGE_SIZE, "a guard page separates two ranges");
    }

    check(!is_vmalloc_addr(&ARENA), "a kernel address outside the window is not mistaken for one");
    check(vmalloc(0, ours::mm::GAF_KERNEL) == nullptr, "a zero-sized request is refused");
    check(vmalloc(VMALLOC_SIZE + PAGE_SIZE, ours::mm::GAF_KERNEL) == nullptr,
          "a request larger than the window is refused");

    vfree(odd);
    vfree(one);
    vfree(nullptr);

    check(vmalloc_free_pages() == before, "every page and address is back");
    if (failed) {
        log::error("vmalloc: self test failed {} check(s)", failed);
    } else {
        log::info("vmalloc: self test passed");
    }
}

} // namespace heap
