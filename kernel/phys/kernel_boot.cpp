#include <arch/random.hpp>
#include <arch/tick.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/boot_options.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/kernel_boot.hpp>
#include <ours/phys/elf_image.hpp>
#include <ours/phys/kernel_package_loader.hpp>
#include <ours/phys/print.hpp>
#include <ustl/mem/align.hpp>

#include <cstring>

namespace ours::phys {

auto KernelBoot::gen_load_bias(usize const limit) -> usize {
    if (limit < PAGE_SIZE) {
        return 0;
    }
    usize offset;
    if (!arch::rand(&offset)) {
        offset = arch::Tick::get().tsc;
    }
    return ustl::mm::align_down(offset % limit, PAGE_SIZE);
}

auto KernelBoot::init(KernelPackageLoader &kpackage, BootMem *const bootmem) -> void {
    bootmem_ = bootmem;

    auto const status = kimage_.init(kpackage.kernel());
    if (status != Status::Ok) {
        panic("Failed to initialize kernel image: {}", static_cast<u32>(status));
    }

    package_handoff_ = kpackage.handoff();

    if (!global_boot_options()->aslr_disabled) {
        kaslr_offset_ = gen_load_bias(GB(1));
        // Do check
    } else {
        kaslr_offset_ = 0;
    }
}

auto KernelBoot::alloc_top_down(usize const size, usize const alignment, char const *const what)
    -> PhysAddr {
    auto const allocation = bootmem_->allocate({
        .size = size,
        .alignment = alignment,
        .lower = bootmem_->min_address(),
        .upper = bootmem_->max_address(),
        .node = bootmem::kAnyNode,
        .direction = bootmem::AllocationDirection::TopDown,
    });
    if (allocation.is_err()) {
        panic("No memory to place {}", what);
    }
    return allocation.unwrap().base;
}

auto KernelBoot::load() -> void {
    auto const image_size = ustl::mm::align_up(kimage_.size(), PAGE_SIZE);
    auto const target_addr = alloc_top_down(image_size, PAGE_SIZE, "kernel image");
    std::memset(reinterpret_cast<void *>(target_addr), 0, image_size);

    auto result = kimage_.load_at(target_addr);
    if (result.is_err()) {
        panic("Failed to load image at {}, given a reason: {}", target_addr,
              static_cast<u32>(result.unwrap_err()));
    }

    kernel_entry_ = result.unwrap();
    kernel_size_ = image_size;
    kernel_addr_ = target_addr;
}

auto KernelBoot::build_kernel_aspace() -> void {
    auto aspace = global_aspace();
    DEBUG_ASSERT(aspace, "Global aspace must be initialized");
    auto const target_base = KERNEL_LOAD_BASE + kaslr_offset_;
    auto const fix_status = kimage_.fix_mapping(*aspace, target_base);
    if (fix_status != Status::Ok) {
        panic("Failed to map kernel ELF segments: {}", static_cast<u32>(fix_status));
    }

    kernel_entry_ = kernel_entry_ - kernel_addr_ + target_base;

    constexpr auto PHYSMAP_MMU_FLAGS = MmuFlags::PermMask | MmuFlags::Present;
    auto const physmap_result =
        aspace->map(KERNEL_ASPACE_BASE, ARCH_PHYSMAP_SIZE / PAGE_SIZE, 0, PHYSMAP_MMU_FLAGS);
    if (physmap_result.is_err()) {
        panic("Failed to map physmap area");
    }
}

auto KernelBoot::boot() -> void {
    build_kernel_aspace();
    do_handoff();
}

WEAK_LINK
auto KernelBoot::arch_do_handoff() -> void {}

auto KernelBoot::do_handoff() -> void {
    auto const handoff = global_handoff();
    DEBUG_ASSERT(handoff, "Global handoff must be initialized");

    // Snapshot the final memory map into its own bootmem-owned buffer --
    // once vmours reads it, nothing here reaches back into phys again.
    // extra_reservations=1: alloc_top_down() below reserves the buffer's
    // own range before snapshot() runs, so the estimate has to include it.
    auto const max_entries = bootmem_->max_snapshot_entries(/*extra_reservations=*/1);
    auto const buffer = alloc_top_down(max_entries * sizeof(bootmem::MemRange),
                                       alignof(bootmem::MemRange), "the boot memory map snapshot");
    auto *const entries = reinterpret_cast<bootmem::MemRange *>(buffer);
    auto const snapshot =
        bootmem_->snapshot({entries, max_entries});
    if (snapshot.is_err()) {
        panic("Boot memory map snapshot overflowed its own capacity estimate");
    }

    handoff->mem.ranges = snapshot.unwrap();
    handoff->mem.kernel_load_addr = kernel_addr_;
    handoff->boot_package = package_handoff_;

    invoke_kernel(handoff);
}

} // namespace ours::phys
