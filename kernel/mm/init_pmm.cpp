#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/memmap_init.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/panic.hpp>
#include <ours/status.hpp>

#include <algorithm>

#include <arch/x86/cache.hpp>
#include <heap/kmalloc.hpp>
#include <ktl/memory_resource.hpp>
#include <logz4/log.hpp>
#include <ustl/bit.hpp>
#include <ustl/lazy_init.hpp>
#include <ustl/limits.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/views/span.hpp>
namespace ours::mm {
/// Declared `extern` in init.hpp and moved forward by set_init_stage();
/// this is the one definition of it. Starts at BootStrap, which is where
/// the kernel is before init_pmm() runs.
InitStage INIT_STAGE = InitStage::BootStrap;

namespace {
    /// vmours never shares physload's live BootMem: it reads the flat
    /// memory map physload hands off once and rebuilds its own
    /// physical-memory bookkeeping from it, replaying the same
    /// add_usable()/reserve() calls physload itself used to build that
    /// map in the first place.  Capacity mirrors x86/phys's own
    /// BootMem storage (see kernel/arch/x86/phys/memory.cpp).
    constexpr usize MAX_EARLY_MEMORY_REGIONS = 128;

    INIT_DATA
    bootmem::MemoryRegion USABLE_STORAGE[MAX_EARLY_MEMORY_REGIONS];
    INIT_DATA
    bootmem::Reservation RESERVATION_STORAGE[MAX_EARLY_MEMORY_REGIONS];
    INIT_DATA
    ustl::LazyInit<bootmem::PhysicalMemory> KERNEL_BOOTMEM;
} // namespace

INIT_CODE
auto init_pmm(ustl::views::Span<Pfn> max_zone_pfn) -> void {
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

    // Descriptors first, nodes second. build_memmap() only needs the boot
    // allocator's ranges, while a PmZone binds its run of descriptors as it is
    // constructed -- so building the memmap after the zones is what forced
    // PmZone to defer that binding to first use. It no longer has to.
    ASSERT(Status::Ok == build_memmap(), "Failed to build the memmap");

    auto const bootstrap_status = PmNode::bootstrap(max_zone_pfn);
    ASSERT(bootstrap_status == Status::Ok);

    // Last, because it needs the zones: which zone a pfn falls in is what it
    // stamps into each descriptor.
    init_memmap();
    MemMap::get().dump();

    // The handover itself, and the stage that says so, in the same breath.
    //
    // do_handoff() gives the buddy every range bootmem still calls free without
    // marking those ranges used in bootmem, so from this line on EarlyMem and
    // the buddy would otherwise both believe they own the same frames. Anything
    // that still asked EarlyMem -- alloc_page_table(), for all of
    // CpuLocal::init() -- got memory the buddy was free to hand out again.
    // Announcing the stage here is what makes pmm_enabled() mean what its name
    // says; see InitStage::Pmm.
    EarlyMem::do_handoff();
    set_init_stage(InitStage::Pmm);
}

/// The object caches and kmalloc, on top of the page allocator init_pmm() left
/// running.
///
/// Split out of init_pmm() because neither is the physical memory manager, and
/// keeping them there hid a real ordering constraint inside a function whose
/// name did not mention it. CpuLocal::init() has to have run before this: every
/// MemCache::init() creates reaches CpuLocal::allocate<CpuCache>(). start_kernel
/// now drives that order in the open, one init level each.
///
/// Named for the cache rather than for the heap because the VMM has a heap of
/// its own -- vmalloc, init_vmm.cpp's init_heap() -- which comes much later,
/// after there is an address space to carve from.
INIT_CODE
auto init_cache() -> void {
    // Every object cache is built on these two; without them no allocation
    // after this point can work, so there is nothing to fall back to.
    if (auto const status = MemCache::init(); status != Status::Ok) {
        panic("mm: the object caches' own caches could not be made: {}", to_string(status));
    }

    heap::init_kmalloc();
    // The allocator-aware containers in `ustl::collections` default to a
    // hosted new/delete resource that does not exist here; point them at the
    // kernel heap now that it is up.
    ktl::init_memory_resource();

    // The heap exists now, which is what MemMap::Impl::allocate() waits for:
    // its non-early path is `new`, not a page allocation, so it needs this
    // stage rather than InitStage::Pmm. The two used to be the same value, and
    // that is the whole of the bug described on InitStage::Pmm -- one stage
    // cannot answer both "the buddy owns the frames" (true since
    // do_handoff(), well before this point) and "`new` works" (true only
    // here).
    set_init_stage(InitStage::Heap);
}

INIT_CODE
auto handoff_early_pmm(phys::MemoryHandoff const &mh) -> Status {
    // mh.ranges is split at every reservation boundary, so its own size
    // is not the right bound to check here -- add_usable()/reserve()
    // below coalesce adjacent same-node regions as they go, so what
    // actually has to fit USABLE_STORAGE/RESERVATION_STORAGE is the
    // number of *distinct* usable/reserved regions, which each call
    // reports for itself via its return status.
    auto *const bootmem = KERNEL_BOOTMEM.init(bootmem::PhysicalMemoryStorage{
        .usable = USABLE_STORAGE,
        .reservations = RESERVATION_STORAGE,
    });
    // Every entry -- free or not -- is RAM physload knew about, so it is all
    // usable(); a claimed entry is additionally reserved(), same as physload's
    // own construction from an OBI memory item (kernel/phys/memory.cpp's
    // register_range).
    //
    // entry.type carries the claim across verbatim: this is the only moment the
    // kernel can learn that a given range holds a page table it is about to
    // walk rather than a hole in RAM, because the phys program that knew is
    // gone. Reconstructing it later is not possible, which is why the type
    // travels with the range instead of being inferred from addresses here.
    for (auto const &entry: mh.ranges) {
        if (bootmem->add_usable(entry.range, 0) != Status::Ok) {
            return Status::BadState;
        }
        if (entry.used() && bootmem->reserve(entry.range, entry.type) != Status::Ok) {
            return Status::BadState;
        }
    }

    EarlyMem::BOOTMEM = bootmem;
    mm::KERNEL_PHYS_BASE = mh.kernel_load_addr;
    // Where the image actually landed after KASLR: physboot relocated it, so
    // the linker symbol already carries the runtime address.
    mm::KERNEL_VIRT_BASE = VirtAddr(IMAGE_START);

    mm::priv::MAX_PHYS_ADDR = bootmem->max_address();
    mm::priv::MIN_PHYS_ADDR = bootmem->min_address();

    return Status::Ok;
}

/// Not INIT_CODE, for the reason spelt out over EarlyMem::reclaim_init_area().
auto free_init_area(InitArea) -> void {
    EarlyMem::reclaim_init_area();
}

} // namespace ours::mm
