#include <ours/mm/early_mem.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/memory_model.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/phys/handoff.hpp>

#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>
#include <algorithm>

namespace ours::mm {
    INIT_CODE
    auto EarlyMem::dump() -> void {
        log::trace("Memory information from BootMem");
        log::trace("|Domain | {:18} | {:18}", "Base", "Size");
        [[maybe_unused]]
        auto const status = visit_usable(min_address(), max_address(), bootmem::kAnyNode,
            [] (bootmem::MemoryRegion const region) {
                log::trace("|{:6x} | 0x{:16X} | 0x{:16X}", region.node, region.range.base, region.range.size);
                return true;
            });
        DEBUG_ASSERT(status == Status::Ok, "Failed to enumerate boot memory");
    }

    INIT_CODE
    auto EarlyMem::get_pfn_range_node(NnId nid) -> gktl::Range<Pfn> {
        auto start = ustl::NumericLimits<Pfn>::max();
        auto end = Pfn {0};
        auto const status = visit_usable(min_address(), max_address(), nid,
            [&start, &end] (bootmem::MemoryRegion const region) {
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
        auto nr_presents = usize {0};
        [[maybe_unused]]
        auto const status = visit_usable(pfn_to_phys(start), pfn_to_phys(end), bootmem::kAnyNode,
            [&nr_presents] (bootmem::MemoryRegion const region) {
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
            usize order = MAX_FRAME_ORDER;
            while (start + BIT(order) > end) {
                order -= 1;
            }

            free_frames(pfn_to_frame(start), order);
            start += BIT(order);
        }
    }

    INIT_CODE FORCE_INLINE 
    static auto free_frames_phys_range(PhysAddr start, PhysAddr end) -> void {
        auto const start_pfn = phys_to_pfn(start);
        auto const end_pfn = std::min(phys_to_pfn(end), max_pfn());
        if (start_pfn < end_pfn) {
            free_frames_pfn_range(start_pfn, end_pfn);
        }
    }

    INIT_CODE
    static auto free_unused_memory() -> void {
        [[maybe_unused]]
        auto const status = EarlyMem::visit_free(EarlyMem::min_address(), EarlyMem::max_address(),
            bootmem::kAnyNode, [] (bootmem::MemoryRegion const region) {
                free_frames_phys_range(region.range.base, region.range.end());
                return true;
            });
        DEBUG_ASSERT(status == Status::Ok, "Failed to release early memory");
    }

    INIT_CODE
    auto EarlyMem::do_handoff() -> void {
        free_unused_memory();
    }

    INIT_CODE
    auto EarlyMem::reclaim_init_area() -> void {
        auto start = PhysMap::virt_to_phys(kKernelInitStart);
        auto end = PhysMap::virt_to_phys(kKernelInitEnd);

        if (end < start || end - start < PAGE_SIZE) {
            return;
        }

        free_frames_phys_range(start, end);
    }

} // namespace ours::mm
