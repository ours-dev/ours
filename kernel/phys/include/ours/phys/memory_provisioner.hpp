/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_MEMORY_PROVISIONER_HPP
#define OURS_PHYS_MEMORY_PROVISIONER_HPP 1

#include <ours/phys/arch_bootmem.hpp>
#include <ours/phys/boot_package_input.hpp>
#include <ours/types.hpp>

#include <span>

#include <obi/container_view.hpp>
#include <obi/memory_config_view.hpp>

namespace ours::phys {
class Aspace;

/// The OBI→BootMem adapter.  BootMem itself stays policy-free (it neither
/// prints, panics, nor knows the obi:: vocabulary), so the early-boot memory
/// policy -- which firmware ranges count as usable, what to do when the OBI
/// container carries no memory item at all, and the reservation order that
/// keeps the boot image and the boot package off the allocator's hands --
/// lives here, holding the one BootMem it drives.
///
/// The four steps run in this order and no other: the arch hook that
/// install() runs allocates through BootMem, so it must see every
/// reservation already in place.
///
/// Not PHYS_EXPORT: only physload's obi_main() drives this class, and
/// memory.cpp links into physload.core alone.
class MemoryProvisioner final {
  public:
    explicit MemoryProvisioner(BootMem &memory) : m_memory(memory) {}

    /// Memory discovery.  Registers the typed OBI `memory` item when the
    /// container carries one, and falls back to `legacy_ranges` through the
    /// `arch_detect_memory` hook only when there is **no memory item at
    /// all**.  An item that is present but carries no Ram range is an
    /// invalid configuration, not a fallback.  Panics on an invalid config
    /// -- this is early boot, there is nothing to return to.
    auto detect(obi::ContainerView const &package, std::span<obi::MemoryRange const> legacy_ranges)
        -> void;

    /// Reserve [PHYS_LOAD_START, PHYS_LOAD_END) -- this image.
    auto reserve_boot_image() -> void;

    /// Reserve the outer OBI package the boot loader supplied.
    auto reserve_package(BootPackageInput const &input) -> void;

    /// Bind the aspace (the ASPACE global, null-guarded), run the arch hook
    /// (unconditionally -- the arch implementation handles a null aspace
    /// itself), then page-align the arena.
    auto install(Aspace *aspace) -> void;

  private:
    BootMem &m_memory;
};

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_MEMORY_PROVISIONER_HPP
