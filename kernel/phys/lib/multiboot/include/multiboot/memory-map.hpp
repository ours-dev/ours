/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef MULTIBOOT_MEMORY_MAP_HPP
#define MULTIBOOT_MEMORY_MAP_HPP 1

#include <multiboot/view.hpp>

#include <bootmem/physical-memory.hpp>

#include <limits>

namespace multiboot {
    using ours::PhysAddr;

    /// Registers loader-provided RAM before protecting every other entry.
    /// This deliberately makes overlapping firmware entries order-independent.
    [[nodiscard]] inline auto apply_memory_map(View::MemoryMap const &map,
                                                bootmem::PhysicalMemory &memory) -> Status {
        auto const native_address = [](u64 const value) -> ustl::Result<PhysAddr, Status> {
            if (value > std::numeric_limits<PhysAddr>::max()) {
                return ustl::err(Status::OutOfRange);
            }
            return ustl::ok(static_cast<PhysAddr>(value));
        };

        auto has_available_memory = false;
        for (usize index = 0; index < map.count(); ++index) {
            auto const entry = map.range(index);
            if ((entry.type != MultibootMmapEntry::Available
                 && entry.type != MultibootMmapEntry::AcpiReclaimable)
                || entry.len == 0) {
                continue;
            }
            auto const base = native_address(entry.addr);
            auto const size = native_address(entry.len);
            if (base.is_err() || size.is_err()) {
                return Status::OutOfRange;
            }
            auto const range = bootmem::Range::from(base.unwrap(), size.unwrap());
            if (range.is_err()) {
                return range.unwrap_err();
            }
            if (auto const status = memory.add_usable(range.unwrap(), 0); status != Status::Ok) {
                return status;
            }
            has_available_memory = has_available_memory
                || entry.type == MultibootMmapEntry::Available;
        }
        if (!has_available_memory) {
            return Status::InvalidArgument;
        }

        for (usize index = 0; index < map.count(); ++index) {
            auto const entry = map.range(index);
            if (entry.type == MultibootMmapEntry::Available || entry.len == 0) {
                continue;
            }
            auto const base = native_address(entry.addr);
            auto const size = native_address(entry.len);
            if (base.is_err() || size.is_err()) {
                return Status::OutOfRange;
            }
            auto const range = bootmem::Range::from(base.unwrap(), size.unwrap());
            if (range.is_err()) {
                return range.unwrap_err();
            }
            if (auto const status = memory.reserve(range.unwrap()); status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }
} // namespace multiboot

#endif // MULTIBOOT_MEMORY_MAP_HPP
