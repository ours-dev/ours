/// physboot — the default phys module that boots the Ours kernel (vmours)
///
/// physload loads this module out of the kernel package, binds its imports
/// against physload's export table and runs its static constructors, then
/// calls the entry declared by PHYS_MODULE() at the bottom of this file.
/// From here physboot resolves the kernel ELF inside the `kernel.name`
/// BootFS directory, loads it, applies KASLR relocation, and hands off to
/// the kernel.

#include "physboot.hpp"

#include <ours/arch/aspace_layout.hpp>
#include <ours/panic.hpp>
#include <ours/phys/arch_handoff.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/boot_options.hpp>
#include <ours/phys/elf_image.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/kernel_package_loader.hpp>
#include <ours/phys/module.hpp>

#include <string_view>

#include <cstring>

#include "detail/boot_alloc.hpp"
#include <arch/random.hpp>
#include <arch/tick.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

namespace ours::phys {
namespace {
    using std::operator""sv;

    /// A page-aligned KASLR bias of up to `limit`, from whatever entropy the
    /// architecture offers (the TSC as the fallback).
    auto gen_load_bias(usize const limit) -> usize {
        if (limit < PAGE_SIZE) {
            return 0;
        }
        usize entropy;
        if (!arch::rand(&entropy)) {
            entropy = arch::Tick::get().tsc;
        }
        return ustl::mm::align_down(entropy % limit, PAGE_SIZE);
    }

    /// Place the kernel ELF in physical memory and report where: the
    /// placement base, plus the entry point's offset from it.
    struct KernelPlacement final {
        PhysAddr base;
        usize entry_offset;
    };

    auto load_kernel_image(ElfImage &image) -> KernelPlacement {
        auto const image_pages = ustl::mm::align_up(image.size(), PAGE_SIZE);
        auto const target = detail::allocate_top_down(image_pages, PAGE_SIZE,
                                                     bootmem::MemoryType::KernelImage,
                                                     "kernel image")
                                .base;
        std::memset(reinterpret_cast<void *>(target), 0, image_pages);

        auto const entry = image.load_at(target);
        if (entry.is_err()) {
            panic("Failed to load the kernel image at {}: {}", target,
                  static_cast<u32>(entry.unwrap_err()));
        }
        log::debug("Kernel image at 0x{:X}, {} pages, entry offset 0x{:X}", target,
                   image_pages / PAGE_SIZE, entry.unwrap() - target);
        return {.base = target, .entry_offset = entry.unwrap() - target};
    }

    /// Collects the mappings fix_mapping() makes, for the handoff.
    ///
    /// A whole page rather than a StaticVec in physboot's .bss: the array has to
    /// outlive the jump, because the kernel's mh.mappings is a span into it, and
    /// physboot's own image is MemoryType::PhysImage -- memory the kernel is
    /// free to reuse. The one page is MemoryType::Handoff, the same claim the
    /// memory-map snapshot makes, and for the same reason.
    class MappingRecorder final {
      public:
        MappingRecorder() {
            auto const buffer = detail::allocate_top_down(PAGE_SIZE, PAGE_SIZE,
                                                         bootmem::MemoryType::Handoff,
                                                         "the kernel mapping list");
            m_entries = reinterpret_cast<MemoryMapping *>(buffer.base);
            m_capacity = PAGE_SIZE / sizeof(MemoryMapping);
        }

        /// ElfImage::MappingRecorder. False means the page is full, which fails
        /// the mapping rather than handing the kernel a list with a hole in it.
        static auto record(void *const context, VirtAddr const base, PhysAddr const paddr,
                           usize const size, MappingPerms const perms) -> bool {
            auto *const self = static_cast<MappingRecorder *>(context);
            if (self->m_count >= self->m_capacity) {
                return false;
            }
            self->m_entries[self->m_count++] = {
                .base = base,
                .paddr = paddr,
                .size = size,
                .perms = perms,
            };
            return true;
        }

        [[nodiscard]]
        auto entries() const -> ustl::views::Span<MemoryMapping const> {
            return {m_entries, m_count};
        }

      private:
        MemoryMapping *m_entries{};
        usize m_capacity{};
        usize m_count{};
    };

    /// Map the kernel's segments at its linked (plus KASLR) virtual base and
    /// the physmap window, all into the aspace physload established -- the
    /// one whose root the kernel adopts as its own CR3.
    ///
    /// Only the image's own segments are recorded for the handoff. The physmap
    /// below is not: its address and size are architectural constants
    /// (ARCH_PHYSMAP_VIRT_BASE, ARCH_PHYSMAP_SIZE) that the kernel compiles in
    /// and phys reads from the same header, so telling it would be telling it
    /// what it already knows. The image's layout is the opposite case -- where
    /// it landed depends on KASLR and on how the ELF was split, neither of which
    /// the kernel witnessed.
    auto build_kernel_aspace(ElfImage &image, VirtAddr const load_base,
                             MappingRecorder *const recorder) -> void {
        auto *const aspace = global_aspace();
        auto const fix_status =
            image.fix_mapping(*aspace, load_base, &MappingRecorder::record, recorder);
        if (fix_status != Status::Ok) {
            panic("Failed to map kernel ELF segments: {}", static_cast<u32>(fix_status));
        }

        // Writable but never executable.  The physmap aliases every
        // physical page, the kernel image included, so an executable
        // physmap would hand back an executable alias of every read-only
        // and non-executable mapping fix_mapping() just made -- W^X on the
        // kernel's own virtual addresses would mean nothing.
        constexpr auto PHYSMAP_MMU_FLAGS =
            MmuFlags::Readable | MmuFlags::Writable | MmuFlags::Present;
        auto const physmap =
            aspace->map(KERNEL_ASPACE_BASE, ARCH_PHYSMAP_SIZE / PAGE_SIZE, 0, PHYSMAP_MMU_FLAGS);
        if (physmap.is_err()) {
            panic("Failed to map physmap area");
        }
    }

    /// Snapshot the final memory map into a bootmem-owned buffer -- once
    /// vmours reads it, nothing here reaches back into phys again.
    /// extra_reservations=1: the buffer's own range is reserved by the
    /// allocate() below before snapshot() runs, so the estimate has to
    /// include it.
    ///
    /// Whole pages, at page alignment, and not alignof(MemRange): this buffer
    /// outlives the jump -- the kernel's mh.ranges points into it -- so its
    /// reservation has to be something the kernel can describe, and downstream
    /// of the handoff the unit of everything is a frame. Asking for eight-byte
    /// alignment produced a reservation that was a sub-page tail of a page it
    /// shared with free memory ([0x3FE9DBE0, 0x3FE9E000)), which no frame
    /// descriptor can represent: the page is neither wholly free nor wholly
    /// handoff, and init_memmap()'s own walk had not even initialised it.
    auto snapshot_memory_map() -> std::span<bootmem::MemRange const> {
        auto const bootmem = global_bootmem();
        auto const max_entries = bootmem->max_snapshot_entries(/*extra_reservations=*/1);
        auto const bytes =
            ustl::mm::align_up(max_entries * sizeof(bootmem::MemRange), usize(PAGE_SIZE));
        auto const buffer = detail::allocate_top_down(bytes, PAGE_SIZE,
                                                     bootmem::MemoryType::Handoff,
                                                     "the boot memory map snapshot");
        auto *const entries = reinterpret_cast<bootmem::MemRange *>(buffer.base);
        auto const snapshot = bootmem->snapshot({entries, max_entries});
        if (snapshot.is_err()) {
            panic("Boot memory map snapshot overflowed its own capacity estimate");
        }
        return snapshot.unwrap();
    }
} // namespace

[[noreturn]]
auto physboot_init(PhysLoadHandoff const &handoff) -> void {
    // 1. View the kernel-package BootFS physload materialized — no outer
    //    OBI re-parsing, no decompression, no scratch ownership here.
    auto const storage = KernelPackageLoader::Bytes(
        reinterpret_cast<u8 const *>(handoff.kernel_storage_base), handoff.kernel_storage_size);

    // 2. Resolve the kernel ELF inside the `kernel.name` directory
    //    (default "vmours"; file names are KernelPackage's fixed vocabulary).
    KernelPackageLoader kpackage;
    auto const package_status =
        kpackage.init(storage, handoff.obi_package_base, handoff.obi_package_size,
                      handoff.boot_options->kernel_name());
    if (package_status != Status::Ok) {
        panic("Invalid kernel package: {}", static_cast<u32>(package_status));
    }

    // 3. Initialize, load and relocate the kernel image.
    //    TODO(SmallHuaZi): apply code-patching metadata
    //    (<kernel_name>/code-patches) to the loaded image.
    ElfImage image;
    auto const init_status = image.init(kpackage.kernel());
    if (init_status != Status::Ok) {
        panic("Failed to initialize the kernel image: {}", static_cast<u32>(init_status));
    }

    auto const placement = load_kernel_image(image);

    auto kaslr_offset = usize{0};
    if (!handoff.boot_options->aslr_disabled()) {
        kaslr_offset = gen_load_bias(GB(1));
    }
    auto const load_base = VirtAddr(KERNEL_LOAD_BASE + kaslr_offset);
    if (auto const status = image.relocate(load_base); status != Status::Ok) {
        panic("Failed to relocate the kernel image: {}", static_cast<u32>(status));
    }

    // Before build_kernel_aspace(): the recorder takes its page from bootmem,
    // and every bootmem allocation has to happen before snapshot_memory_map()
    // or the kernel never learns the range is taken.
    MappingRecorder recorder;
    build_kernel_aspace(image, load_base, &recorder);

    // 4. Fill the kernel-side handoff: memory map, placement, boot package.
    auto *const kernel_handoff = global_handoff();
    kernel_handoff->mem.ranges = snapshot_memory_map();
    kernel_handoff->mem.kernel_load_addr = placement.base;
    kernel_handoff->mem.mappings = recorder.entries();
    for (auto const &mapping: kernel_handoff->mem.mappings) {
        log::debug("Kernel mapping [0x{:X}, 0x{:X}) <- 0x{:X} {}{}{}", mapping.base,
                   mapping.base + mapping.size, mapping.paddr,
                   !!(mapping.perms & MappingPerms::Read) ? "r" : "-",
                   !!(mapping.perms & MappingPerms::Write) ? "w" : "-",
                   !!(mapping.perms & MappingPerms::Exec) ? "x" : "-");
    }
    kernel_handoff->boot_package = kpackage.handoff();

    // 5. Hand off to the kernel. Never returns.
    log::info("handoff: jumping to kernel at 0x{:X}, cr3=0x{:X}",
              load_base + placement.entry_offset, global_aspace()->pgd());
    arch_handoff_to_kernel(VirtAddr(load_base + placement.entry_offset), global_aspace()->pgd(),
                           kernel_handoff);
}

} // namespace ours::phys

PHYS_MODULE("physboot", ours::phys::physboot_init);
