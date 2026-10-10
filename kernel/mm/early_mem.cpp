#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/phys/handoff.hpp>

#include <algorithm>

#include <logz4/log.hpp>
#include <ustl/bit.hpp>
#include <ustl/mem/align.hpp>

namespace ours::mm {
INIT_CODE
auto EarlyMem::dump() -> void {
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

INIT_CODE
auto EarlyMem::get_pfn_range_node(NnId nid) -> gktl::Range<Pfn> {
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

INIT_CODE
auto EarlyMem::count_present_frames(Pfn start, Pfn end) -> usize {
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

INIT_CODE
static auto free_frames_pfn_range(Pfn start, Pfn end) -> void {
    while (start < end) {
        // Two separate limits, and the alignment one is not optional. A buddy
        // block of order N is identified by its lowest pfn and finds its partner
        // at `pfn ^ (1 << N)`; that identity only holds if the block is aligned to
        // 2^N pages. Hand the allocator a misaligned block and the merge walk
        // reaches for a "buddy" that belongs to some other block entirely --
        // which removes a live frame from its list and loses it. A range that
        // starts anywhere but on a MAX_FRAME_ORDER boundary is the normal case,
        // so this is not a corner.
        //
        // ustl::countr_zero(start) is the largest power of two `start` is
        // aligned to; the second limit then shrinks that to what is left in the
        // range.
        auto order = std::min(usize(MAX_FRAME_ORDER), usize(ustl::countr_zero(start)));
        while (start + (usize{1} << order) > end) {
            order -= 1;
        }

        free_frames(pfn_to_frame(start), order);
        start += usize{1} << order;
    }
}

/// Frees the whole frames inside [start, end), and returns how many.
///
/// The start rounds *up* and the end down. A free range can begin or end in
/// the middle of a frame whose other part is reserved: bootmem keeps
/// reservations to the byte, and the boot image, for one, ends mid-page --
/// 0x900 bytes into its last page on the rel image. Rounding the start down,
/// as this did, handed that whole frame to the buddy, and the next owner could
/// overwrite the image's tail while the kernel still read it (userboot_init()
/// reads it, then gives its frames to userboot). A partial frame is not free,
/// so it is not given away.
INIT_CODE FORCE_INLINE
static auto free_frames_phys_range(PhysAddr start, PhysAddr end) -> usize {
    auto const start_pfn = phys_to_pfn(ustl::mm::align_up(start, PAGE_SIZE));
    auto const end_pfn = std::min(phys_to_pfn(end), max_pfn());
    if (start_pfn >= end_pfn) {
        return 0;
    }
    // The guard that would have caught the rounding above: every reserved
    // frame was stamped before this runs (stamp_reserved_frames()), and none
    // may reach the buddy. The tail page of a reservation that ends mid-frame
    // carries the stamp, since the stamping rounds the end up.
    if constexpr (KERNEL_DEBUG_LEVEL > 0) {
        for (auto pfn = start_pfn; pfn < end_pfn; ++pfn) {
            DEBUG_ASSERT(!pfn_valid(pfn) || !(pfn_to_frame(pfn)->flags() & page::Flags::Reserved),
                         "frame {:#x} is reserved but inside a free range", pfn_to_phys(pfn));
        }
    }
    free_frames_pfn_range(start_pfn, end_pfn);
    return end_pfn - start_pfn;
}

INIT_CODE
static auto free_unused_memory() -> void {
    usize nr_regions = 0;
    usize nr_frames = 0;

    [[maybe_unused]]
    auto const status =
        EarlyMem::visit_free(EarlyMem::min_address(), EarlyMem::max_address(), bootmem::ANY_NODE,
                             [&](bootmem::MemoryRegion const region) {
                                 nr_regions += 1;
                                 nr_frames +=
                                     free_frames_phys_range(region.range.base, region.range.end());
                                 return true;
                             });
    DEBUG_ASSERT(status == Status::Ok, "Failed to release early memory");

    // The one line that says the buddy actually got something. Everything after
    // this point allocates from it, so a zero here turns into a hang somewhere
    // far away -- the first caller simply retries forever waiting for a reclaim
    // that cannot happen this early.
    log::info("Released {} regions, {} frames ({} MiB) to the buddy allocator", nr_regions,
              nr_frames, (nr_frames * PAGE_SIZE) >> 20);
}

/// Hands every range bootmem still calls free over to the page allocator on the
/// books as well as in fact, so that bootmem has nothing left to give.
///
/// free_unused_memory() above hands the buddy every range bootmem still calls
/// free -- and then leaves bootmem's own bitmap untouched, so bootmem goes on
/// believing those ranges are free. Both allocators then think they own the
/// same frames, and the loser is whoever allocated first: the frame gets handed
/// out a second time and written over. That is not theory. alloc_page_table()
/// used to run in exactly that window and take page tables from here; the next
/// per-CPU unit allocated with Gaf::Zero landed on a live leaf table and blanked
/// it, and the boot died several thousand instructions later writing to a
/// virtual address whose PTE had evaporated (see docs/changes/260925/mm.md).
///
/// The window is closed now -- pmm_enabled() becomes true on the line after
/// do_handoff() -- but "nobody asks any more" is a property of the callers, and
/// the next one that does will corrupt memory just as quietly. This makes it a
/// property of bootmem instead: a reservation covering everything means
/// find_free() finds nothing, so a late allocate() returns null and its caller's
/// existing null check turns it into a visible failure.
///
/// One free range at a time, taken from the front, rather than a walk that
/// reserves as it goes: a reservation changes what the free view has left to
/// show, so mutating inside the walk would skip ranges. Sixteen or so iterations
/// over a list of the same length, once, at boot.
///
/// The type is MemoryType::Mmu and not a blanket reservation over
/// [min_address, max_address): with typed reservations a single range covering
/// everything would overlap the phys image, the page tables and the firmware's
/// holes, all of which are already reserved as something else -- and reserve()
/// refuses that on purpose, because retyping someone else's claim is how the
/// information gets lost. Claiming only what was actually released also leaves
/// each range saying who has it, which is what the kernel reads on the other
/// side.
///
/// Throwing bootmem's arrays away altogether would reach the same state from
/// the other direction: a late allocate() would then dereference null rather
/// than allocate a frame twice in silence.
INIT_CODE
static auto retire_bootmem() -> void {
    for (;;) {
        auto first = bootmem::Range{};
        auto found = false;
        [[maybe_unused]]
        auto const status = EarlyMem::visit_free(
            EarlyMem::min_address(), EarlyMem::max_address(), bootmem::ANY_NODE,
            [&](bootmem::MemoryRegion const region) {
                first = region.range;
                found = true;
                // The first one is enough: reserving it is what makes the next
                // call see the one after.
                return false;
            });
        DEBUG_ASSERT(status == Status::Ok, "Failed to enumerate the released memory");
        if (!found) {
            return;
        }

        auto const reserved =
            EarlyMem::reserve(first.base, first.size, bootmem::MemoryType::Mmu);
        // A hard failure, not a log line. Carrying on would leave bootmem
        // willing to hand out memory the buddy owns, which is the state this
        // exists to end -- and the loop would not terminate either.
        ASSERT(Status::Ok == reserved, "Failed to retire the boot allocator: {}",
               static_cast<u32>(reserved));
    }
}

INIT_CODE
auto EarlyMem::do_handoff() -> void {
    free_unused_memory();
    retire_bootmem();
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
/// The rule, then: every INIT_CODE frame has unwound before the reclaim
/// happens, and whatever calls this is plain text itself.
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

    // These frames are the kernel image's, stamped Reserved at boot
    // (stamp_reserved_frames()), and counted so on their node and zone. They
    // go to the buddy as any other free frame only once they are off those
    // books: unwire_frame() clears Reserved and the reserved counts first,
    // and the free comes after.
    auto const start_pfn = phys_to_pfn(ustl::mm::align_up(start, PAGE_SIZE));
    auto const end_pfn = std::min(phys_to_pfn(end), max_pfn());
    for (auto pfn = start_pfn; pfn < end_pfn; ++pfn) {
        auto *const frame = pfn_to_frame(pfn);
        if (!!(frame->flags() & page::Flags::Reserved)) {
            unwire_frame(frame);
        }
    }

    free_frames_phys_range(start, end);
}

} // namespace ours::mm
