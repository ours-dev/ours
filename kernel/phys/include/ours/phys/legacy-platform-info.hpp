/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_LEGACY_PLATFORM_INFO_HPP
#define OURS_PHYS_LEGACY_PLATFORM_INFO_HPP 1

#include <obi/package-format.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>

namespace ours::phys {

/// Platform data boot-shim hands physload alongside the OBI container:
/// everything physload's arch-neutral fallback paths (arch_detect_memory,
/// ACPI RSDP) need that has not yet been converted into a real OBI item
/// (see .agents/tasks/phys-module-and-obi-tree/DESIGN.md).  Memory ranges
/// are spelled in obi::MemoryRange on purpose -- the same vocabulary
/// init_memory() already reads from a real OBI memory item -- so that
/// conversion, when it lands, changes only *how* this data is delivered,
/// not its shape.
///
/// This is not a module-ABI boundary: boot-shim and physload are always
/// built together from the same source tree (there is exactly one
/// physload for one boot-shim, unlike a physload_module(), which can be
/// any of several and can be built independently later), so the ordinary
/// same-build guarantee already covers this struct's layout.  It should
/// shrink to nothing once the OBI-item conversion lands.
struct LegacyPlatformInfo {
    ustl::Option<PhysAddr> acpi_rsdp;
    obi::MemoryRange const *memory_ranges;
    usize memory_range_count;
};

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_LEGACY_PLATFORM_INFO_HPP
