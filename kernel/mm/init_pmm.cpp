#include <ours/mm/init.hpp>

#include <ours/mm/early-mem.hpp>
#include <ours/mm/pm-zone.hpp>
#include <ours/mm/pm-node.hpp>
#include <ours/mm/object-cache.hpp>
#include <ours/mm/memory-model.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vmm.hpp>

#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/panic.hpp>
#include <ours/status.hpp>
#include <ours/mm/cpu-local.hpp>

#include <ustl/bit.hpp>
#include <ustl/limits.hpp>
#include <ustl/views/span.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/algorithms/minmax.hpp>
#include <ustl/algorithms/generation.hpp>

#include <ktl/kmalloc.hpp>

#include <logz4/log.hpp>

#include <arch/x86/cache.hpp>

using ustl::algorithms::min;
using ustl::algorithms::max;
using ustl::algorithms::clamp;

namespace ours::mm {
    INIT_CODE
    auto init_pmm(ustl::views::Span<Pfn> max_zone_pfn) -> void {
        ASSERT(!max_zone_pfn.empty());
    
        log::debug("Available regions in BootMem"); 
        auto const visit_status = EarlyMem::visit_free(EarlyMem::min_address(), EarlyMem::max_address(),
            bootmem::kAnyNode, [] (bootmem::MemoryRegion const region) {
                log::debug("    None-{}| [0x{:X}, 0x{:X})", region.node,
                    region.range.base, region.range.end());
                return true;
            });
        ASSERT(visit_status == Status::Ok);

        auto const bootstrap_status = PmNode::bootstrap(max_zone_pfn);
        ASSERT(bootstrap_status == Status::Ok);

        MemoryModel::get().init(true);
        MemoryModel::get().dump();

        EarlyMem::do_handoff();

        MemCache::init();

        ktl::init_kmalloc();

        // Ok, PMM is available now.
        set_init_stage(InitStage::Pmm);
    }

    INIT_CODE
    auto handoff_early_pmm(phys::MemoryHandoff const &mh) -> Status {
        EarlyMem::s_bootmem = &mh.bootmem;
        mm::g_kernel_phys_base = mh.kernel_load_addr;

        mm::priv::g_max_phys_addr = EarlyMem::s_bootmem->max_address;
        mm::priv::g_min_phys_addr = EarlyMem::s_bootmem->min_address;

        // Verifies the signature of bootmem
        return Status::Ok; 
    }

    INIT_CODE
    auto reclaim_init_area(RemTag) -> Status {
        EarlyMem::reclaim_init_area();
        return Status::Ok;
    }

} // namespace ours::mm
