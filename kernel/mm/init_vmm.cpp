#include <ours/arch/aspace_layout.hpp>
#include <ours/assert.hpp>
#include <ours/boot/handoff.hpp>
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

#include <algorithm>

#include <arch/x86/cache.hpp>
#include <heap/vmalloc.hpp>
#include <logz4/log.hpp>
#include <ustl/lazy_init.hpp>
#include <ustl/limits.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/views/span.hpp>

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

    /// The ranges the VMM records that physboot's mapping list does not cover.
    ///
    /// Everything the kernel image occupies used to be listed here too, copied
    /// from the linker symbols (KERNEL_CODE_START and friends). It is gone:
    /// physboot reports what it actually mapped, KASLR offset and RELRO splits
    /// included, and enrol_handoff_mappings() below reads that instead. What is
    /// left is the two ranges phys does not describe, for opposite reasons.
    INIT_CONST
    PhysVma PRESET_VMAS[] = {
        // The physmap. phys does map it, but its base and size are
        // architectural constants out of aspace_layout.hpp -- the same header
        // phys reads -- so there is nothing to be told.
        //
        // If ASLR is ever applied to it, this needs to fix up with
        // get_kernel_virt_base().
        {.name = "k:physmap",
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
        // Not in phys's mapping list either, and it could not be: the units are
        // placed per NUMA node, and the topology is parsed in the kernel long
        // after the handoff.
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

    /// A name for a mapping, from what it permits.
    ///
    /// physboot's list carries permissions and no name, deliberately: the `k:`
    /// vocabulary is the kernel's, and it follows from the permissions anyway.
    /// The answers are the same three words the hand-written table used, and
    /// they no longer claim to know which *section* a range holds -- a claim
    /// that table was making on the strength of a symbol, while the mapping it
    /// described had already been split by RELRO into runs no section boundary
    /// explains.
    [[nodiscard]]
    INIT_CODE
    auto name_of(phys::MappingPerms const perms) -> char const * {
        if (!!(perms & phys::MappingPerms::Exec)) {
            return "k:code";
        }
        if (!!(perms & phys::MappingPerms::Write)) {
            return "k:data";
        }
        return "k:rodata";
    }

    [[nodiscard]]
    INIT_CODE
    auto vma_flags_of(phys::MappingPerms const perms) -> VmaFlags {
        auto flags = VmaFlags::None;
        if (!!(perms & phys::MappingPerms::Read)) {
            flags |= VmaFlags::Read;
        }
        if (!!(perms & phys::MappingPerms::Write)) {
            flags |= VmaFlags::Write;
        }
        if (!!(perms & phys::MappingPerms::Exec)) {
            flags |= VmaFlags::Exec;
        }
        return flags;
    }

    /// Claims and re-protects every range physboot mapped for the kernel.
    ///
    /// This replaces a table written from the kernel's own linker symbols. The
    /// symbols say where sections were linked; the list says what physboot
    /// established, KASLR bias and RELRO clipping included, and only physboot
    /// ever knew that. The list is gapless over the loaded image, which is the
    /// property the old table's comment demanded and could only assert: a page
    /// inside the image that no VMA claims is a page the VMM may hand out to
    /// something else while the image still occupies it.
    INIT_CODE
    auto enrol_handoff_mappings() -> void {
        auto const mappings = boot::handoff().mem.mappings;
        ASSERT(!mappings.empty(), "physboot handed over no kernel mappings");

        for (auto const &mapping: mappings) {
            entrol_preset_vma({
                .name = name_of(mapping.perms),
                .base = mapping.base,
                .size = mapping.size,
                .vmaf = vma_flags_of(mapping.perms),
                .backing = PhysVma::Backing::Mapped,
            });
        }
    }

    INIT_CODE
    auto init_vmm_preheap() -> void {
        VmAspace::init_kernel_aspace();

        enrol_handoff_mappings();
        for (usize i = 0; i < std::size(PRESET_VMAS); ++i) {
            entrol_preset_vma(PRESET_VMAS[i]);
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
    auto init_heap() -> void {
        // The window is not in PHYS_VMOS above, deliberately. Everything in
        // that table is a range something else already owns and the VMM merely
        // records; the vmalloc window is the VMM's own, and vmalloc creates the
        // sub-VMA itself so that the area it maps into is the area it holds.
        // It runs here, after init_vmm_preheap(), because there is no root VMA
        // to carve from before that.
        heap::init_vmalloc();

        // Immediately, and in both images: the allocator has no other caller
        // yet, and a boot that reaches userspace is not evidence that it
        // works. It costs three allocations at boot and is the only thing
        // standing between a broken vmalloc and the first subsystem to depend
        // on it.
        heap::vmalloc_self_test();
    }
} // namespace

/// Requires:
///     1).
INIT_CODE
auto init_vmm() -> void {
    init_vmm_preheap();

    init_heap();
}
} // namespace ours::mm