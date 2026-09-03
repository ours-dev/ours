#include <arch/x86/cache.hpp>
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_page.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/panic.hpp>
#include <ours/status.hpp>
#include <ustl/lazy_init.hpp>
#include <ustl/limits.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/views/span.hpp>

#include <algorithm>

namespace ours::mm {
namespace {
    struct PhysVma {
        char const *name;
        VirtAddr base;
        usize size;
        VmaFlags vmaf;
    };

    [[maybe_unused]]
    constexpr auto PRESET_VMA_MMU_FLAGS = MmuFlags::PermMask;

    /// Used in init_vmm_postheap.
    /// TODO(SmallHuaZi) These be provided by PhysBoot through HandOff rather than
    /// direct to use linking symbols.
    /// In image order, and gapless: every page between IMAGE_START and
    /// IMAGE_END has to be claimed here, or the VMM is free to hand that
    /// address out again to something else while the loaded image still
    /// occupies it.
    INIT_CONST
    PhysVma PHYS_VMOS[] = {
        // The ELF head: .dynamic, plus the tables physboot read the image
        // through (.dynsym, .rela.dyn, the build-ID note).
        {.name = "k:head",
         .base = VirtAddr(IMAGE_START),
         .size = usize(KERNEL_CODE_START - IMAGE_START),
         .vmaf = VmaFlags::Read | VmaFlags::Write},
        {.name = "k:code",
         .base = VirtAddr(KERNEL_CODE_START),
         .size = usize(KERNEL_CODE_END - KERNEL_CODE_START),
         .vmaf = VmaFlags::Exec | VmaFlags::Read},
        {.name = "k:rodata",
         .base = VirtAddr(KERNEL_RODATA_START),
         .size = usize(KERNEL_RODATA_END - KERNEL_RODATA_START),
         .vmaf = VmaFlags::Read},
        {.name = "k:data",
         .base = VirtAddr(KERNEL_DATA_START),
         .size = usize(KERNEL_DATA_END - KERNEL_DATA_START),
         .vmaf = VmaFlags::Write | VmaFlags::Read},
        // Writable only until physboot finished relocating, and mapped
        // read-only ever since.
        {.name = "k:relro",
         .base = VirtAddr(KERNEL_RELRO_START),
         .size = usize(KERNEL_RELRO_END - KERNEL_RELRO_START),
         .vmaf = VmaFlags::Read},
        {.name = "k:bss",
         .base = VirtAddr(KERNEL_BSS_START),
         .size = usize(KERNEL_BSS_END - KERNEL_BSS_START),
         .vmaf = VmaFlags::Read | VmaFlags::Write},
        {.name = "k:init",
         .base = VirtAddr(KERNEL_INIT_START),
         .size = usize(KERNEL_INIT_END - KERNEL_INIT_START),
         .vmaf = VmaFlags::Read | VmaFlags::Write | VmaFlags::Exec},
        {.name = "k:physmap",
         // If ASLR(Address space layout randomilization) enabled, this needs to fix up with get_kernel_virt_base().
         .base = PhysMap::VIRT_BASE,
         .size = PhysMap::SIZE,
         .vmaf = VmaFlags::Read | VmaFlags::Write},
        {.name = "k:padding",
         .base = PhysMap::VIRT_BASE + PhysMap::SIZE,
         // Reserve a max page to prevent over prevent overwring.
         .size = MAX_PAGE_SIZE,
         .vmaf = VmaFlags::Read},
    };

    INIT_CODE auto entrol_preset_vma(PhysVma const &phys_vma) -> void {
        auto [base, end] = resolve_page_range(phys_vma.base, phys_vma.base + phys_vma.size);
        log::trace("Reserve area {}: [{:X}, {:X})", phys_vma.name, base, end);

        auto kaspace = VmAspace::kernel_aspace();
        auto root_vma = kaspace->root_vma();
        ustl::Arc<VmArea> vma;
        [[maybe_unused]]
        auto status =
            root_vma->create_subvma(phys_vma.base - root_vma->base(), phys_vma.size, phys_vma.vmaf,
                                    phys_vma.name, VmMapOption::Fixed, &vma);
        DEBUG_ASSERT(Status::Ok == status);

        status = vma->reserve(0, phys_vma.size, extract_permissions(phys_vma.vmaf), phys_vma.name);
        DEBUG_ASSERT(Status::Ok == status);
    }

    INIT_CODE auto init_vmm_preheap() -> void {
        VmAspace::init_kernel_aspace();

        for (usize i = 0; i < std::size(PHYS_VMOS); ++i) {
            entrol_preset_vma(PHYS_VMOS[i]);
        }

        auto frame = alloc_frames(GAF_KERNEL, usize(0));
        if (!frame) {
            panic("Failed to allocate the zero page");
        }
        // Claim it for the VMM: the frame comes straight from the buddy
        // allocator, so this is a role *declaration*, not a query.
        frame->become<page::VmmBody>();
        ZERO_PAGE = frame;
        ZERO_PAGE->mark_pinned();

        VmAspace::kernel_aspace()->root_vma()->dump();
    }

    /// Placeholder for vmalloc.
    ///
    /// Empty on purpose, and not the kernel object heap -- that is
    /// mm::init_cache(), which runs several init levels earlier, before the VMM
    /// exists at all. This one is the VMM's own allocator: virtually
    /// contiguous, physically scattered, and therefore impossible to build
    /// before the address space it carves from. init_vmm_preheap() above is
    /// named for being the part that runs before *this*.
    INIT_CODE auto init_heap() -> void {}
} // namespace

/// Requires:
///     1).
INIT_CODE auto init_vmm() -> void {
    init_vmm_preheap();

    init_heap();
}
} // namespace ours::mm