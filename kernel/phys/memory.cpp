#include <ours/mm/cfg.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/memory_provisioner.hpp>

#include <limits>

#include <logz4/log.hpp>
#include <obi/memory_config_view.hpp>

namespace ours::phys {
extern char const PHYS_LOAD_START[] LINK_NAME("__executable_start");
extern char const PHYS_LOAD_END[] LINK_NAME("__executable_end");

Aspace *ASPACE;

namespace {
    /// Validate and register one range against BootMem.  Shared by
    /// MemoryProvisioner::detect() (decoding a real OBI memory item) and
    /// arch_detect_memory()'s default body (boot-shim's platform snapshot)
    /// so the two delivery mechanisms for the same obi::MemoryRange
    /// vocabulary stay behaviorally identical.  A TU-local free function,
    /// not a member: the WEAK default body below needs it too.  `added_usable`,
    /// when given, is set on a successfully-registered Ram range; only
    /// detect() needs to know that (to reject an item with no Ram range at
    /// all).
    [[nodiscard]]
    auto register_range(BootMem &memory, obi::MemoryRange const &value,
                        bool *const added_usable = nullptr) -> Status {
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

        switch (static_cast<obi::MemoryRangeType>(value.type)) {
            case obi::MemoryRangeType::Ram: {
                auto const status = memory.add_usable(physical.unwrap(), 0);
                if (status == ours::Status::Ok && added_usable != nullptr) {
                    *added_usable = true;
                }
                return status;
            }
            case obi::MemoryRangeType::Peripheral:
                return memory.reserve(physical.unwrap(), bootmem::MemoryType::Peripheral);
            case obi::MemoryRangeType::Reserved:
                return memory.reserve(physical.unwrap(), bootmem::MemoryType::FirmwareReserved);
        }
        return ours::Status::InvArgs;
    }
} // namespace

WEAK_LINK
auto arch_detect_memory(std::span<obi::MemoryRange const> const ranges) -> void {
    auto *const memory = global_bootmem();
    for (auto const &value: ranges) {
        auto const status = register_range(*memory, value);
        if (status != ours::Status::Ok) {
            panic("Legacy platform memory range could not be registered: {}",
                  static_cast<u32>(status));
        }
    }
}

auto MemoryProvisioner::detect(obi::ContainerView const &package,
                               std::span<obi::MemoryRange const> const legacy_ranges) -> void {
    auto const memory = package.find("memory");
    if (memory.is_err()) {
        log::debug("OBI memory item is ambiguous ({})", static_cast<u32>(memory.unwrap_err()));
        panic("Invalid OBI memory configuration: {}", static_cast<u32>(memory.unwrap_err()));
    }
    if (memory.unwrap().is_none()) {
        // No typed memory item at all: the architecture's legacy fallback
        // is the only source of a map.  (An item that is present but
        // carries no Ram range is an error, handled below -- not this.)
        arch_detect_memory(legacy_ranges);
        return;
    }

    auto const ranges = obi::MemoryConfigView::from(memory.unwrap().value());
    if (ranges.is_err()) {
        log::debug("OBI memory item is invalid ({})", static_cast<u32>(ranges.unwrap_err()));
        panic("Invalid OBI memory configuration: {}", static_cast<u32>(ranges.unwrap_err()));
    }

    auto added_usable = false;
    for (usize index = 0; index < ranges.unwrap().count(); ++index) {
        auto range = ranges.unwrap().range(index);
        if (range.is_err()) {
            log::debug("OBI memory range {} is invalid", index);
            panic("Invalid OBI memory configuration: {}", static_cast<u32>(range.unwrap_err()));
        }
        auto const status = register_range(m_memory, range.unwrap(), &added_usable);
        if (status != ours::Status::Ok) {
            log::debug("OBI memory range {} could not be registered", index);
            panic("Invalid OBI memory configuration: {}", static_cast<u32>(status));
        }
    }
    if (!added_usable) {
        panic("Invalid OBI memory configuration: {}", static_cast<u32>(ours::Status::InvArgs));
    }
}

auto MemoryProvisioner::reserve_boot_image() -> void {
    auto const phys_start = PhysAddr(PHYS_LOAD_START);
    auto const phys_end = PhysAddr(PHYS_LOAD_END);
    auto const image = bootmem::Range::from(phys_start, phys_end - phys_start);
    if (image.is_err() ||
        m_memory.reserve(image.unwrap(), bootmem::MemoryType::PhysImage) != Status::Ok) {
        panic("Failed to reserve phys image");
    }
}

auto MemoryProvisioner::reserve_package(BootPackageInput const &input) -> void {
    auto const input_status = input.reserve(m_memory);
    if (input_status != Status::Ok) {
        panic("Failed to reserve OBI package: {}", static_cast<u32>(input_status));
    }
}

auto MemoryProvisioner::install(Aspace *aspace) -> void {
    // The null guard is load-bearing asymmetry: ASPACE has readers (physboot)
    // that must not see a null, while arch_init_memory is always called and
    // its implementations decide for themselves what a null aspace means.
    if (aspace != nullptr) {
        ASPACE = aspace;
    }

    arch_init_memory(aspace);
    if (auto const status = m_memory.trim_usable(PAGE_SIZE); status != Status::Ok) {
        panic("Failed to page-align boot memory: {}", static_cast<u32>(status));
    }
}
} // namespace ours::phys
