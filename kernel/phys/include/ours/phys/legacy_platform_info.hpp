/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_LEGACY_PLATFORM_INFO_HPP
#define OURS_PHYS_LEGACY_PLATFORM_INFO_HPP 1

#include <ours/types.hpp>

#include <string_view>

#include <obi/package_format.hpp>
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
/// same-build guarantee already covers this struct's layout.  The memory
/// and ACPI members should disappear once the OBI-item conversion lands;
/// the ones below them cannot, and are documented one by one.
struct LegacyPlatformInfo {
    ustl::Option<PhysAddr> acpi_rsdp;
    obi::MemoryRange const *memory_ranges;
    usize memory_range_count;

    /// The command line the legacy protocol supplied (GRUB's Multiboot2
    /// MULTIBOOT_TAG_TYPE_CMDLINE -- what the user typed in the boot menu),
    /// applied on top of the container's own Config item so a menu entry
    /// can override what was baked in at build time.  Empty if the
    /// protocol carried none.
    ///
    /// This one is not "waiting for an OBI item" the way the two above
    /// are: an OBI container is laid out at build time and cannot be
    /// appended to at run time, so a command line that only exists at boot
    /// has no item to arrive in and never will.
    ///
    /// Phys-only by design.  It reaches BootOptions and stops there -- it is
    /// deliberately not carried into Handoff, because vmours does not read
    /// the bootloader's command line at all; it takes its own options from
    /// the container's Config item (ours::boot::boot_package()).  So this
    /// steers physload and physboot (`kernel.phys.next`, `kernel.name`,
    /// `aslr.disabled`) and nothing past them.
    std::string_view cmdline;

    /// What the bootloader called itself (MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME).
    /// Diagnostic only -- physload logs it so a boot transcript says which
    /// loader produced it.  Empty if the protocol carried none.
    std::string_view bootloader_name;

    /// Physical address of the flattened device tree the firmware passed, on
    /// a platform that describes itself with one rather than with ACPI.
    /// None where there is no tree.
    ///
    /// Only the address travels: physload forwards it into Handoff and the
    /// kernel walks the tree itself, as it walks the ACPI tables from
    /// acpi_rsdp.  Like the command line it has no OBI item to wait for --
    /// the firmware writes the tree at boot, after the container was built.
    /// The shim that found it lists the blob's own range as reserved in
    /// memory_ranges, so nothing allocates over it before the kernel reads it.
    ustl::Option<PhysAddr> device_tree;
};

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_LEGACY_PLATFORM_INFO_HPP
