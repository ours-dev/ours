#include <arch/random.hpp>
#include <arch/tick.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/boot-options.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/kernel-boot.hpp>
#include <ours/phys/kernel-image.hpp>
#include <ours/phys/kernel-package-loader.hpp>
#include <ours/phys/print.hpp>
#include <ustl/mem/align.hpp>

#include <cstring>

namespace ours::phys {
namespace {
    auto handoff_reserve(void *context, bootmem::Range const range) -> Status {
        return static_cast<BootMem *>(context)->reserve(range);
    }

    auto handoff_assign_node(void *context, bootmem::Range const range, NnId const node) -> Status {
        return static_cast<BootMem *>(context)->assign_node(range, node);
    }

    auto handoff_allocate(void *context, bootmem::AllocationRequest const request,
                          bootmem::Range *const result) -> Status {
        if (result == nullptr) {
            return Status::InvalidArgument;
        }
        auto const allocation = static_cast<BootMem *>(context)->allocate(request);
        if (allocation.is_err()) {
            return allocation.unwrap_err();
        }
        *result = allocation.unwrap();
        return Status::Ok;
    }

    auto handoff_release(void *context, bootmem::Range const range) -> Status {
        return static_cast<BootMem *>(context)->release(range);
    }

    auto handoff_visit(void *context, bootmem::MemoryQuery const query,
                       bootmem::MemoryVisitor const visitor, void *const visitor_context)
        -> Status {
        return static_cast<BootMem *>(context)->visit(query, visitor, visitor_context);
    }
} // namespace

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

auto KernelBoot::init(KernelPackageLoader &kpackage) -> void {
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

auto KernelBoot::load() -> void {
    auto const image_size = ustl::mm::align_up(kimage_.size(), PAGE_SIZE);
    auto const mem = global_bootmem();
    auto const allocation = mem->allocate({
        .size = image_size,
        .alignment = PAGE_SIZE,
        .lower = mem->min_address(),
        .upper = mem->max_address(),
        .node = bootmem::kAnyNode,
        .direction = bootmem::AllocationDirection::TopDown,
    });
    if (allocation.is_err()) {
        panic("No enough memory to place image");
    }
    auto const target_addr = allocation.unwrap().base;
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

    constexpr auto const kPhysmapMmuFlags = MmuFlags::PermMask | MmuFlags::Present;
    auto const physmap_result =
        aspace->map(KERNEL_ASPACE_BASE, ARCH_PHYSMAP_SIZE / PAGE_SIZE, 0, kPhysmapMmuFlags);
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

    auto const mem = global_bootmem();
    handoff->mem.bootmem = {
        .context = mem,
        .reserve = handoff_reserve,
        .assign_node = handoff_assign_node,
        .allocate = handoff_allocate,
        .release = handoff_release,
        .visit = handoff_visit,
        .min_address = mem->min_address(),
        .max_address = mem->max_address(),
    };

    handoff->mem.kernel_load_addr = kernel_addr_;
    handoff->boot_package = package_handoff_;

    invoke_kernel(handoff);
}

} // namespace ours::phys
