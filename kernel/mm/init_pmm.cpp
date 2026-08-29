#include <arch/x86/cache.hpp>
#include <ktl/kmalloc.hpp>
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/memory_model.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/panic.hpp>
#include <ours/status.hpp>
#include <ustl/bit.hpp>
#include <ustl/lazy_init.hpp>
#include <ustl/limits.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/views/span.hpp>

#include <algorithm>
namespace ours::mm {
/// Declared `extern` in init.hpp and moved forward by set_init_stage();
/// this is the one definition of it. Starts at BootStrap, which is where
/// the kernel is before init_pmm() runs.
InitStage g_init_stage = InitStage::BootStrap;

namespace {
    /// vmours never shares physload's live BootMem: it reads the flat
    /// memory map physload hands off once and rebuilds its own
    /// physical-memory bookkeeping from it, replaying the same
    /// add_usable()/reserve() calls physload itself used to build that
    /// map in the first place.  Capacity mirrors x86/phys's own
    /// BootMem storage (see kernel/arch/x86/phys/memory.cpp).
    constexpr usize MAX_EARLY_MEMORY_REGIONS = 128;

    INIT_DATA bootmem::MemoryRegion s_usable_storage[MAX_EARLY_MEMORY_REGIONS];
    INIT_DATA bootmem::Range s_reservation_storage[MAX_EARLY_MEMORY_REGIONS];
    INIT_DATA ustl::LazyInit<bootmem::PhysicalMemory> s_kernel_bootmem;
} // namespace

INIT_CODE auto init_pmm(ustl::views::Span<Pfn> max_zone_pfn) -> void {
    ASSERT(!max_zone_pfn.empty());

    log::debug("Available regions in BootMem");
    auto const visit_status =
        EarlyMem::visit_free(EarlyMem::min_address(), EarlyMem::max_address(), bootmem::ANY_NODE,
                             [](bootmem::MemoryRegion const region) {
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

    // Must sit between the buddy allocator coming up and the slab being
    // built: it allocates the per-CPU units from the buddy, and the slab
    // cannot be initialised before it, because MemCache::init() reaches
    // CpuLocal::allocate<CpuCache>() for every cache it creates.
    CpuLocal::init();

    MemCache::init();

    ktl::init_kmalloc();

    // Ok, PMM is available now.
    set_init_stage(InitStage::Pmm);

    // And say so. MemoryModel::Impl::allocate() reads this to choose
    // between the real allocator and EarlyMem; nothing had ever set it,
    // so that choice always fell to EarlyMem.
    g_pmm_enabled = true;
}

INIT_CODE auto handoff_early_pmm(phys::MemoryHandoff const &mh) -> Status {
    // mh.ranges is split at every reservation boundary, so its own size
    // is not the right bound to check here -- add_usable()/reserve()
    // below coalesce adjacent same-node regions as they go, so what
    // actually has to fit s_usable_storage/s_reservation_storage is the
    // number of *distinct* usable/reserved regions, which each call
    // reports for itself via its return status.
    auto *const bootmem = s_kernel_bootmem.init(bootmem::PhysicalMemoryStorage{
        .usable = s_usable_storage,
        .reservations = s_reservation_storage,
    });
    // Every entry -- free or not -- is RAM physload knew about, so it
    // is all usable(); the entries flagged used are additionally
    // reserved(), same as physload's own construction from an OBI
    // memory item (kernel/phys/memory.cpp's apply_memory_range).
    for (auto const &entry: mh.ranges) {
        if (bootmem->add_usable(entry.range, 0) != Status::Ok) {
            return Status::BadState;
        }
        if (entry.used && bootmem->reserve(entry.range) != Status::Ok) {
            return Status::BadState;
        }
    }

    EarlyMem::s_bootmem = bootmem;
    mm::g_kernel_phys_base = mh.kernel_load_addr;

    mm::priv::MAX_PHYS_ADDR = bootmem->max_address();
    mm::priv::MIN_PHYS_ADDR = bootmem->min_address();

    return Status::Ok;
}

INIT_CODE auto free_init_area(InitArea) -> void {
    EarlyMem::reclaim_init_area();
}

} // namespace ours::mm
