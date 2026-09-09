#include <arch/x86/cache.hpp>
#include <logz4/log.hpp>
#include <ours/arch/aspace_layout.hpp>
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
        /// What already stands behind the range when the VMM comes up. It
        /// decides how far entrol_preset_vma() goes: every entry claims its
        /// range, but only a Mapped one has page tables to stamp.
        enum class Backing {
            /// Mapped before the VMM existed -- by physboot, or by the PMM --
            /// and mapped for good. The VMA claims the range and rewrites the
            /// permissions of the mappings already there, which is how the
            /// image's text ends up read-only and its data non-executable.
            Mapped,

            /// Address space only. Another subsystem maps into the range on
            /// its own, one piece at a time, and stays the owner of those
            /// pages. The VMA exists so the VMM never hands the range out to
            /// anyone else; it has no business in the page tables behind it,
            /// and cannot even assume they are populated.
            AddressSpace,
        };

        char const *name;
        VirtAddr base;
        usize size;
        VmaFlags vmaf;
        Backing backing;
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
         .vmaf = VmaFlags::Read | VmaFlags::Write,
         .backing = PhysVma::Backing::Mapped},
        {.name = "k:code",
         .base = VirtAddr(KERNEL_CODE_START),
         .size = usize(KERNEL_CODE_END - KERNEL_CODE_START),
         .vmaf = VmaFlags::Exec | VmaFlags::Read,
         .backing = PhysVma::Backing::Mapped},
        {.name = "k:rodata",
         .base = VirtAddr(KERNEL_RODATA_START),
         .size = usize(KERNEL_RODATA_END - KERNEL_RODATA_START),
         .vmaf = VmaFlags::Read,
         .backing = PhysVma::Backing::Mapped},
        {.name = "k:data",
         .base = VirtAddr(KERNEL_DATA_START),
         .size = usize(KERNEL_DATA_END - KERNEL_DATA_START),
         .vmaf = VmaFlags::Write | VmaFlags::Read,
         .backing = PhysVma::Backing::Mapped},
        // Writable only until physboot finished relocating, and mapped
        // read-only ever since.
        {.name = "k:relro",
         .base = VirtAddr(KERNEL_RELRO_START),
         .size = usize(KERNEL_RELRO_END - KERNEL_RELRO_START),
         .vmaf = VmaFlags::Read,
         .backing = PhysVma::Backing::Mapped},
        {.name = "k:bss",
         .base = VirtAddr(KERNEL_BSS_START),
         .size = usize(KERNEL_BSS_END - KERNEL_BSS_START),
         .vmaf = VmaFlags::Read | VmaFlags::Write,
         .backing = PhysVma::Backing::Mapped},
        {.name = "k:init",
         .base = VirtAddr(KERNEL_INIT_START),
         .size = usize(KERNEL_INIT_END - KERNEL_INIT_START),
         .vmaf = VmaFlags::Read | VmaFlags::Write | VmaFlags::Exec,
         .backing = PhysVma::Backing::Mapped},
        {.name = "k:physmap",
         // If ASLR(Address space layout randomilization) enabled, this needs to fix up with get_kernel_virt_base().
         .base = PhysMap::VIRT_BASE,
         .size = PhysMap::SIZE,
         .vmaf = VmaFlags::Read | VmaFlags::Write,
         .backing = PhysVma::Backing::Mapped},
        // The per-CPU window (ARCH_CPU_LOCAL_VIRT_BASE in aspace_layout.hpp).
        // CpuLocal::Mapper populates it itself, a chunk slot at a time and
        // only as far as the CPUs need, so most of it is never mapped and
        // none of it is the VMM's to protect -- hence AddressSpace, and the
        // full window rather than the part in use today, so a later chunk
        // does not land on something the VMM has since given out.
        //
        // The window begins exactly where the physmap ends. A one-page
        // read-only "k:padding" used to stand there as a guard against
        // running off the physmap; once the window moved in, that guard sat
        // on chunk 0, and reserve() write-protected the page holding CPU 0's
        // per-CPU counters. The next spin lock acquire faulted on its
        // counter update. There is no room for such a guard in this layout,
        // and the physmap is bounded by its own VMA in any case.
        {.name = "k:cpulocal",
         .base = VirtAddr(ARCH_CPU_LOCAL_VIRT_BASE),
         .size = usize(ARCH_CPU_LOCAL_SIZE),
         .vmaf = VmaFlags::Read | VmaFlags::Write,
         .backing = PhysVma::Backing::AddressSpace},
        // The vmalloc window (ARCH_VMALLOC_VIRT_BASE in aspace_layout.hpp).
        //
        // AddressSpace for the same reason as k:cpulocal above: mm::vmalloc()
        // maps into it itself, a range at a time, and owns those pages. Most
        // of the window is unmapped at any moment, so there is nothing here
        // for reserve() to stamp permissions onto -- it would fault walking
        // page tables that do not exist.
        //
        // Claimed in full at boot rather than grown on demand, because the
        // point of the VMA is to stop VmPlacement::anywhere() from handing
        // this range to anyone else. is_vmalloc_addr() answers for the whole
        // window, so anything else living inside it would be freed as though
        // it were a vmalloc allocation.
        {.name = "k:vmalloc",
         .base = VirtAddr(ARCH_VMALLOC_VIRT_BASE),
         .size = usize(ARCH_VMALLOC_SIZE),
         .vmaf = VmaFlags::Read | VmaFlags::Write,
         .backing = PhysVma::Backing::AddressSpace},
    };

    INIT_CODE
    auto entrol_preset_vma(PhysVma const &phys_vma) -> void {
        auto [base, end] = resolve_page_range(phys_vma.base, phys_vma.base + phys_vma.size);
        log::trace("Reserve area {}: [{:X}, {:X})", phys_vma.name, base, end);

        auto kaspace = VmAspace::kernel_aspace();
        auto root_vma = kaspace->root_vma();
        ustl::Arc<VmArea> vma;
        [[maybe_unused]]
        auto status =
            root_vma->create_subvma(VmPlacement::at(phys_vma.base), phys_vma.size, phys_vma.vmaf,
                                    phys_vma.name, VmMapOption::None, &vma);
        DEBUG_ASSERT(Status::Ok == status);

        if (PhysVma::Backing::AddressSpace == phys_vma.backing) {
            // Claimed, and that is all: nothing below this VMA is ours to map
            // or protect. See PhysVma::Backing.
            return;
        }

        status = vma->reserve(vma->base(), phys_vma.size, extract_permissions(phys_vma.vmaf),
                              phys_vma.name);
        DEBUG_ASSERT(Status::Ok == status);
    }

    INIT_CODE
    auto init_vmm_preheap() -> void {
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
    INIT_CODE
    auto init_heap() -> void {}
} // namespace

/// Requires:
///     1).
INIT_CODE
auto init_vmm() -> void {
    init_vmm_preheap();

    init_heap();
}
} // namespace ours::mm