#include <obi/memory-config-view.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/print.hpp>

#include <limits>

namespace ours::phys {
extern char const kPhysLoadStart[] LINK_NAME("__executable_start");
extern char const kPhysLoadEnd[] LINK_NAME("__executable_end");

Aspace *g_aspace;

using MemoryInitializationResult = ustl::Result<bool, ours::Status>;

auto obi_init_memory(obi::ContainerView const &package) -> MemoryInitializationResult {
    auto const memory = package.find("memory");
    if (memory.is_err()) {
        dprintln("OBI memory item is ambiguous ({})", static_cast<u32>(memory.unwrap_err()));
        return ustl::err(memory.unwrap_err());
    }
    if (memory.unwrap().is_none()) {
        return ustl::ok(false);
    }

    auto const ranges = obi::MemoryConfigView::from(memory.unwrap().value());
    if (ranges.is_err()) {
        dprintln("OBI memory item is invalid ({})", static_cast<u32>(ranges.unwrap_err()));
        return ustl::err(ranges.unwrap_err());
    }

    auto const mem = global_bootmem();
    auto added_usable = false;
    for (usize index = 0; index < ranges.unwrap().count(); ++index) {
        auto range = ranges.unwrap().range(index);
        if (range.is_err()) {
            dprintln("OBI memory range {} is invalid", index);
            return ustl::err(range.unwrap_err());
        }
        auto const value = range.unwrap();
        if (value.length == 0 || value.base > std::numeric_limits<u64>::max() - value.length ||
            value.base > std::numeric_limits<usize>::max() ||
            value.length > std::numeric_limits<usize>::max() ||
            value.length > std::numeric_limits<usize>::max() - static_cast<usize>(value.base)) {
            dprintln("OBI memory range {} is out of range", index);
            return ustl::err(ours::Status::OutOfRange);
        }

        auto const physical =
            bootmem::Range::from(PhysAddr(value.base), static_cast<usize>(value.length));
        if (physical.is_err()) {
            dprintln("OBI memory range {} is out of range", index);
            return ustl::err(physical.unwrap_err());
        }

        auto status = ours::Status::InvalidArgument;
        switch (static_cast<obi::MemoryRangeType>(value.type)) {
            case obi::MemoryRangeType::Ram:
                status = mem->add_usable(physical.unwrap(), 0);
                added_usable = status == ours::Status::Ok || added_usable;
                break;
            case obi::MemoryRangeType::Peripheral:
                status = mem->reserve(physical.unwrap());
                break;
            case obi::MemoryRangeType::Reserved:
                status = mem->reserve(physical.unwrap());
                break;
        }
        if (status != ours::Status::Ok) {
            dprintln("OBI memory range {} could not be registered", index);
            return ustl::err(status);
        }
    }
    if (!added_usable) {
        return ustl::err(ours::Status::InvalidArgument);
    }
    return ustl::ok(true);
}

WEAK_LINK
auto arch_detect_memory() -> void {}

namespace {
    auto early_reserve(void *context, bootmem::Range const range) -> Status {
        return static_cast<BootMem *>(context)->reserve(range);
    }

    auto early_assign_node(void *context, bootmem::Range const range, NnId const node) -> Status {
        return static_cast<BootMem *>(context)->assign_node(range, node);
    }

    auto early_allocate(void *context, bootmem::AllocationRequest const request,
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

    auto early_release(void *context, bootmem::Range const range) -> Status {
        return static_cast<BootMem *>(context)->release(range);
    }

    auto early_visit(void *context, bootmem::MemoryQuery const query,
                     bootmem::MemoryVisitor const visitor, void *const visitor_context)
        -> Status {
        return static_cast<BootMem *>(context)->visit(query, visitor, visitor_context);
    }
} // namespace

auto early_mem() -> EarlyMem {
    auto const mem = global_bootmem();
    return {
        .context = mem,
        .reserve = early_reserve,
        .assign_node = early_assign_node,
        .allocate = early_allocate,
        .release = early_release,
        .visit = early_visit,
        .min_address = mem->min_address(),
        .max_address = mem->max_address(),
    };
}

auto init_memory(obi::ContainerView const &package, BootPackageInput const &input, Aspace *aspace)
    -> void {
    auto const mem = global_bootmem();
    auto const initialized = obi_init_memory(package);
    if (initialized.is_err()) {
        panic("Invalid OBI memory configuration: {}", static_cast<u32>(initialized.unwrap_err()));
    }
    if (!initialized.unwrap()) {
        arch_detect_memory();
    }

    auto const phys_start = PhysAddr(kPhysLoadStart);
    auto const phys_end = PhysAddr(kPhysLoadEnd);
    auto const image = bootmem::Range::from(phys_start, phys_end - phys_start);
    if (image.is_err() || mem->reserve(image.unwrap()) != Status::Ok) {
        panic("Failed to reserve phys image");
    }
    auto const input_status = input.reserve(*mem);
    if (input_status != Status::Ok) {
        panic("Failed to reserve OBI package: {}", static_cast<u32>(input_status));
    }

    if (aspace != nullptr) {
        g_aspace = aspace;
    }

    arch_init_memory(aspace);
    if (auto const status = mem->trim_usable(PAGE_SIZE); status != Status::Ok) {
        panic("Failed to page-align boot memory: {}", static_cast<u32>(status));
    }
}
} // namespace ours::phys
