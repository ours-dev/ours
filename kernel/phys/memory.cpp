#include <logz4/log.hpp>
#include <obi/memory_config_view.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/phys/init.hpp>

#include <limits>

namespace ours::phys {
extern char const PHYS_LOAD_START[] LINK_NAME("__executable_start");
extern char const PHYS_LOAD_END[] LINK_NAME("__executable_end");

Aspace *ASPACE;

using MemoryInitializationResult = ustl::Result<bool, ours::Status>;

namespace {
    /// Validate and register one range against BootMem.  Shared by
    /// obi_init_memory() (decoding a real OBI memory item) and
    /// arch_detect_memory()'s callers (boot-shim's platform snapshot) so
    /// the two delivery mechanisms for the same obi::MemoryRange vocabulary
    /// stay behaviorally identical.  `added_usable`, when given, is set on
    /// a successfully-registered Ram range; only obi_init_memory() needs
    /// to know that (to reject an item with no Ram range at all).
    [[nodiscard]]
    auto apply_memory_range(obi::MemoryRange const &value, bool *const added_usable = nullptr)
        -> Status {
        if (value.length == 0 || value.base > std::numeric_limits<u64>::max() - value.length ||
            value.base > std::numeric_limits<usize>::max() ||
            value.length > std::numeric_limits<usize>::max() ||
            value.length > std::numeric_limits<usize>::max() - static_cast<usize>(value.base)) {
            return ours::Status::OutOfRange;
        }

        auto const physical =
            bootmem::Range::from(PhysAddr(value.base), static_cast<usize>(value.length));
        if (physical.is_err()) {
            return physical.unwrap_err();
        }

        auto const mem = global_bootmem();
        switch (static_cast<obi::MemoryRangeType>(value.type)) {
            case obi::MemoryRangeType::Ram: {
                auto const status = mem->add_usable(physical.unwrap(), 0);
                if (status == ours::Status::Ok && added_usable != nullptr) {
                    *added_usable = true;
                }
                return status;
            }
            case obi::MemoryRangeType::Peripheral:
            case obi::MemoryRangeType::Reserved:
                return mem->reserve(physical.unwrap());
        }
        return ours::Status::InvalidArgument;
    }
} // namespace

auto obi_init_memory(obi::ContainerView const &package) -> MemoryInitializationResult {
    auto const memory = package.find("memory");
    if (memory.is_err()) {
        log::debug("OBI memory item is ambiguous ({})", static_cast<u32>(memory.unwrap_err()));
        return ustl::err(memory.unwrap_err());
    }
    if (memory.unwrap().is_none()) {
        return ustl::ok(false);
    }

    auto const ranges = obi::MemoryConfigView::from(memory.unwrap().value());
    if (ranges.is_err()) {
        log::debug("OBI memory item is invalid ({})", static_cast<u32>(ranges.unwrap_err()));
        return ustl::err(ranges.unwrap_err());
    }

    auto added_usable = false;
    for (usize index = 0; index < ranges.unwrap().count(); ++index) {
        auto range = ranges.unwrap().range(index);
        if (range.is_err()) {
            log::debug("OBI memory range {} is invalid", index);
            return ustl::err(range.unwrap_err());
        }
        auto const status = apply_memory_range(range.unwrap(), &added_usable);
        if (status != ours::Status::Ok) {
            log::debug("OBI memory range {} could not be registered", index);
            return ustl::err(status);
        }
    }
    if (!added_usable) {
        return ustl::err(ours::Status::InvalidArgument);
    }
    return ustl::ok(true);
}

WEAK_LINK auto arch_detect_memory(std::span<obi::MemoryRange const> const ranges) -> void {
    for (auto const &value: ranges) {
        auto const status = apply_memory_range(value);
        if (status != ours::Status::Ok) {
            panic("Legacy platform memory range could not be registered: {}",
                  static_cast<u32>(status));
        }
    }
}

auto init_memory(obi::ContainerView const &package, BootPackageInput const &input,
                 std::span<obi::MemoryRange const> const legacy_memory_ranges, Aspace *aspace)
    -> void {
    auto const mem = global_bootmem();
    auto const initialized = obi_init_memory(package);
    if (initialized.is_err()) {
        panic("Invalid OBI memory configuration: {}", static_cast<u32>(initialized.unwrap_err()));
    }
    if (!initialized.unwrap()) {
        arch_detect_memory(legacy_memory_ranges);
    }

    auto const phys_start = PhysAddr(PHYS_LOAD_START);
    auto const phys_end = PhysAddr(PHYS_LOAD_END);
    auto const image = bootmem::Range::from(phys_start, phys_end - phys_start);
    if (image.is_err() || mem->reserve(image.unwrap()) != Status::Ok) {
        panic("Failed to reserve phys image");
    }
    auto const input_status = input.reserve(*mem);
    if (input_status != Status::Ok) {
        panic("Failed to reserve OBI package: {}", static_cast<u32>(input_status));
    }

    if (aspace != nullptr) {
        ASPACE = aspace;
    }

    arch_init_memory(aspace);
    if (auto const status = mem->trim_usable(PAGE_SIZE); status != Status::Ok) {
        panic("Failed to page-align boot memory: {}", static_cast<u32>(status));
    }
}
} // namespace ours::phys
