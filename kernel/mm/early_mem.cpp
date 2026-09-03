#include <logz4/log.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/phys/handoff.hpp>
#include <ustl/bit.hpp>
#include <ustl/mem/align.hpp>

#include <algorithm>

namespace ours::mm {
INIT_CODE auto EarlyMem::dump() -> void {
    log::trace("Memory information from BootMem");
    log::trace("|Domain | {:18} | {:18}", "Base", "Size");
    [[maybe_unused]]
    auto const status = visit_usable(min_address(), max_address(), bootmem::ANY_NODE,
                                     [](bootmem::MemoryRegion const region) {
                                         log::trace("|{:6x} | 0x{:16X} | 0x{:16X}", region.node,
                                                    region.range.base, region.range.size);
                                         return true;
                                     });
    DEBUG_ASSERT(status == Status::Ok, "Failed to enumerate boot memory");
}

INIT_CODE auto EarlyMem::get_pfn_range_node(NnId nid) -> gktl::Range<Pfn> {
    auto start = ustl::NumericLimits<Pfn>::max();
    auto end = Pfn{0};
    auto const status = visit_usable(min_address(), max_address(), nid,
                                     [&start, &end](bootmem::MemoryRegion const region) {
                                         start = std::min(start, phys_to_pfn(region.range.base));
                                         end = std::max(end, phys_to_pfn(region.range.end()));
                                         return true;
                                     });
    if (status != Status::Ok || start == ustl::NumericLimits<Pfn>::max()) {
        return {0, 0};
    }

    return {start, end};
}

INIT_CODE auto EarlyMem::count_present_frames(Pfn start, Pfn end) -> usize {
    auto nr_presents = usize{0};
    [[maybe_unused]]
    auto const status =
        visit_usable(pfn_to_phys(start), pfn_to_phys(end), bootmem::ANY_NODE,
                     [&nr_presents](bootmem::MemoryRegion const region) {
                         if (region.range.size < PAGE_SIZE) {
                             return true;
                         }
                         auto const base = ustl::mm::align_up(region.range.base, PAGE_SIZE);
                         auto const end = ustl::mm::align_down(region.range.end(), PAGE_SIZE);
                         if (end <= base) {
                             return true;
                         }
                         nr_presents += (end - base) / PAGE_SIZE;
                         return true;
                     });
    DEBUG_ASSERT(status == Status::Ok, "Failed to count early-memory frames");

    return nr_presents;
}

INIT_CODE static auto free_frames_pfn_range(Pfn start, Pfn end) -> void {
    while (start < end) {
        // Two separate limits, and the alignment one is not optional. A buddy
        // block of order N is identified by its lowest pfn and finds its partner
        // at `pfn ^ BIT(N)`; that identity only holds if the block is aligned to
        // 2^N pages. Hand the allocator a misaligned block and the merge walk
        // reaches for a "buddy" that belongs to some other block entirely --
        // which removes a live frame from its list and loses it. A range that
        // starts anywhere but on a MAX_FRAME_ORDER boundary is the normal case,
        // so this is not a corner.
        //
        // ustl::countr_zero(start) is the largest power of two `start` is
        // aligned to; the second limit then shrinks that to what is left in the
        // range. Linux writes the same two lines in __free_pages_memory():
        // `order = min(MAX_PAGE_ORDER, __ffs(start))`, then the same loop.
        auto order = std::min(usize(MAX_FRAME_ORDER), usize(ustl::countr_zero(start)));
        while (start + BIT(order) > end) {
            order -= 1;
        }

        free_frames(pfn_to_frame(start), order);
        start += BIT(order);
    }
}

INIT_CODE FORCE_INLINE static auto free_frames_phys_range(PhysAddr start, PhysAddr end) -> void {
    auto const start_pfn = phys_to_pfn(start);
    auto const end_pfn = std::min(phys_to_pfn(end), max_pfn());
    if (start_pfn < end_pfn) {
        free_frames_pfn_range(start_pfn, end_pfn);
    }
}

INIT_CODE static auto free_unused_memory() -> void {
    usize nr_regions = 0;
    usize nr_frames = 0;

    [[maybe_unused]]
    auto const status =
        EarlyMem::visit_free(EarlyMem::min_address(), EarlyMem::max_address(), bootmem::ANY_NODE,
                             [&](bootmem::MemoryRegion const region) {
                                 nr_regions += 1;
                                 nr_frames += (region.range.end() - region.range.base) / PAGE_SIZE;
                                 free_frames_phys_range(region.range.base, region.range.end());
                                 return true;
                             });
    DEBUG_ASSERT(status == Status::Ok, "Failed to release early memory");

    // The one line that says the buddy actually got something. Everything after
    // this point allocates from it, so a zero here turns into a hang somewhere
    // far away -- the first caller simply retries forever waiting for a reclaim
    // that cannot happen this early. Linux prints the same fact from
    // mem_init_print_info() for the same reason.
    log::info("Released {} regions, {} frames ({} MiB) to the buddy allocator", nr_regions,
              nr_frames, (nr_frames * PAGE_SIZE) >> 20);
}

INIT_CODE auto EarlyMem::do_handoff() -> void {
    free_unused_memory();
}

/// Deliberately NOT INIT_CODE, and neither is any frame still on the stack
/// when this runs. It hands the `.init*` sections to the buddy allocator, so
/// anything living there is dead the moment the first later allocation reuses
/// those frames -- and code is not exempt. Marked INIT_CODE, this function
/// would be freeing the instructions it is about to return through, and its
/// caller's too; nothing faults at the free itself, because the mappings stay,
/// so the damage only lands later, silently, wherever the reused frames are
/// written. That is exactly why `labour_routine()` in kernel/top/main.cpp is
/// plain text as well: it calls this and then keeps running.
///
/// Linux draws the same line -- `free_initmem()` is not `__init`, and
/// `kernel_init()`, which calls it, is `__ref` rather than `__init` precisely
/// so that every `__init` frame has unwound (into `kernel_init_freeable()`)
/// before the reclaim happens.
auto EarlyMem::reclaim_init_area() -> void {
    // The `.init*` sections are the tail of the kernel image (kernel.ld), so
    // handing them back leaves physboot's single image allocation contiguous
    // rather than punching a hole in it.  Their addresses are image addresses:
    // PhysMap::virt_to_phys() does not translate those at all, it answers
    // zero, which is what made this a silent no-op.
    auto const start = kernel_image_virt_to_phys(VirtAddr(KERNEL_INIT_START));
    auto const end = kernel_image_virt_to_phys(VirtAddr(KERNEL_INIT_END));

    if (end < start || end - start < PAGE_SIZE) {
        return;
    }

    free_frames_phys_range(start, end);
}

} // namespace ours::mm
