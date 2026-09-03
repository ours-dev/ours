/// physboot — the default phys module that boots the Ours kernel (vmours)
///
/// physload loads this module out of the kernel package, binds its imports
/// against physload's export table and runs its static constructors, then
/// calls the entry declared by PHYS_MODULE() at the bottom of this file.
/// From here physboot resolves the kernel ELF inside the `kernel.name`
/// BootFS directory, loads it, applies KASLR relocation, and hands off to
/// the kernel.

#include "physboot.hpp"

#include <arch/random.hpp>
#include <arch/tick.hpp>
#include <logz4/log.hpp>
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
#include <ustl/mem/align.hpp>

#include <string_view>

#include <cstring>

namespace ours::phys {
namespace {
    using std::operator""sv;

    /// Top-down allocate-or-panic through the live bootmem, spanning its
    /// full range. `what` names the allocation in the panic message.
    auto alloc_top_down(usize const size, usize const alignment, char const *const what)
        -> PhysAddr {
        auto const bootmem = global_bootmem();
        auto const allocation = bootmem->allocate({
            .size = size,
            .alignment = alignment,
            .lower = bootmem->min_address(),
            .upper = bootmem->max_address(),
            .node = bootmem::ANY_NODE,
            .direction = bootmem::AllocationDirection::TopDown,
        });
        if (allocation.is_err()) {
            panic("No memory to place {}", what);
        }
        return allocation.unwrap().base;
    }

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
        auto const target = alloc_top_down(image_pages, PAGE_SIZE, "kernel image");
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

    /// Map the kernel's segments at its linked (plus KASLR) virtual base and
    /// the physmap window, all into the aspace physload established -- the
    /// one whose root the kernel adopts as its own CR3.
    auto build_kernel_aspace(ElfImage &image, VirtAddr const load_base) -> void {
        auto *const aspace = global_aspace();
        auto const fix_status = image.fix_mapping(*aspace, load_base);
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
    auto snapshot_memory_map() -> std::span<bootmem::MemRange const> {
        auto const bootmem = global_bootmem();
        auto const max_entries = bootmem->max_snapshot_entries(/*extra_reservations=*/1);
        auto const buffer =
            alloc_top_down(max_entries * sizeof(bootmem::MemRange), alignof(bootmem::MemRange),
                           "the boot memory map snapshot");
        auto *const entries = reinterpret_cast<bootmem::MemRange *>(buffer);
        auto const snapshot = bootmem->snapshot({entries, max_entries});
        if (snapshot.is_err()) {
            panic("Boot memory map snapshot overflowed its own capacity estimate");
        }
        return snapshot.unwrap();
    }
} // namespace

NO_RETURN auto physboot_init(PhysLoadHandoff const &handoff) -> void {
    // 1. View the kernel-package BootFS physload materialized — no outer
    //    OBI re-parsing, no decompression, no scratch ownership here.
    auto const storage = KernelPackageLoader::Bytes(
        reinterpret_cast<u8 const *>(handoff.kernel_storage_base), handoff.kernel_storage_size);

    // 2. Resolve the kernel ELF inside the `kernel.name` directory
    //    (default "vmours"; file names are KernelPackage's fixed vocabulary).
    KernelPackageLoader kpackage;
    auto const package_status =
        kpackage.init(storage, handoff.obi_package_base, handoff.obi_package_size,
                      handoff.boot_options->kernel_name);
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
    if (!handoff.boot_options->aslr_disabled) {
        kaslr_offset = gen_load_bias(GB(1));
    }
    auto const load_base = VirtAddr(KERNEL_LOAD_BASE + kaslr_offset);
    if (auto const status = image.relocate(load_base); status != Status::Ok) {
        panic("Failed to relocate the kernel image: {}", static_cast<u32>(status));
    }

    build_kernel_aspace(image, load_base);

    // 4. Fill the kernel-side handoff: memory map, placement, boot package.
    auto *const kernel_handoff = global_handoff();
    kernel_handoff->mem.ranges = snapshot_memory_map();
    kernel_handoff->mem.kernel_load_addr = placement.base;
    kernel_handoff->boot_package = kpackage.handoff();

    // 5. Hand off to the kernel. Never returns.
    log::info("handoff: jumping to kernel at 0x{:X}, cr3=0x{:X}",
              load_base + placement.entry_offset, global_aspace()->pgd());
    arch_handoff_to_kernel(VirtAddr(load_base + placement.entry_offset), global_aspace()->pgd(),
                           kernel_handoff);
}

} // namespace ours::phys

PHYS_MODULE("physboot", ours::phys::physboot_init);
