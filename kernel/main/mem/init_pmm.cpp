#include <ours/mem/init.hpp>

#include <ours/mem/early-mem.hpp>
#include <ours/mem/pm-zone.hpp>
#include <ours/mem/pm-node.hpp>
#include <ours/mem/object-cache.hpp>
#include <ours/mem/memory-model.hpp>
#include <ours/mem/physmap.hpp>
#include <ours/mem/pmm.hpp>
#include <ours/mem/vmm.hpp>

#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/panic.hpp>
#include <ours/status.hpp>
#include <ours/mem/cpu-local.hpp>

#include <ustl/bit.hpp>
#include <ustl/limits.hpp>
#include <ustl/views/span.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/algorithms/minmax.hpp>
#include <ustl/algorithms/generation.hpp>

#include <ktl/kmalloc.hpp>

#include <logz4/log.hpp>

#include <arch/cache.hpp>

using ustl::algorithms::min;
using ustl::algorithms::max;
using ustl::algorithms::clamp;

namespace ours::mem {
    INIT_CODE
    auto init_pmm(ustl::views::Span<Pfn> max_zone_pfn) -> void {
        ASSERT(!max_zone_pfn.empty());
    
        log::debug("Available regions in BootMem"); 
        EarlyMem::IterationContext context{bootmem::RegionType::Unused, NUMA_MAX_NODE};
        while (auto region = EarlyMem::iterate(context)) {
            log::debug("    None-{}| [0x{:X}, 0x{:X})", region->nid(), region->base, region->end());
        }

        PmNode::bootstrap(max_zone_pfn);

        MemoryModel::get().init(true);
        MemoryModel::get().dump();

        EarlyMem::do_handoff();

        MemCache::init();

        ktl::init_kmalloc();

        // Ok, PMM is available now.
        set_init_stage(InitStage::Pmm);
    }

    INIT_CODE
    auto handoff_early_pmm(phys::MemoryHandoff &mh) -> Status {
        EarlyMem::s_bootmem = &mh.bootmem;
        mem::g_kernel_phys_base = mh.kernel_load_addr;

        mem::priv::g_max_phys_addr = EarlyMem::s_bootmem->max_address;
        mem::priv::g_min_phys_addr = EarlyMem::s_bootmem->min_address;

        // Verifies the signature of bootmem
        return Status::Ok; 
    }

    INIT_CODE
    auto reclaim_init_area(RemTag) -> Status {
        EarlyMem::reclaim_init_area();
        return Status::Ok;
    }

} // namespace ours::mem