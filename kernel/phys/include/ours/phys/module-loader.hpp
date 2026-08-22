/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_MODULE_LOADER_HPP
#define OURS_PHYS_MODULE_LOADER_HPP 1

#include <obi/boot-obi.hpp>
#include <ours/phys/boot-options.hpp>
#include <ours/phys/kernel-image.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::phys {

/// Loads the next phys module out of the kernel package — the loader half
/// of the physload→physboot split (the object a `physload_module()` GN
/// target produces).
///
/// init() materializes the kernel package (the single StorageKernel item)
/// into a bootmem-owned scratch buffer; load() locates the module named by
/// `kernel.phys.next` in the resulting BootFS (falling back to "physboot"),
/// loads it as an ET_DYN image and applies its relative relocations.  The
/// entry point is the module's `phys_load_handoff`.
class ModuleLoader final {
  public:
    /// Decompress the kernel package into bootmem scratch memory.  The
    /// buffer's ownership moves to the loaded module through
    /// PhysLoadHandoff::kernel_storage_base/size and is never released.
    auto init(obi::BootObi const &boot) -> Status;

    /// Find, load, and relocate the module named by `options.next_module`.
    auto load(BootOptions const &options) -> Status;

    /// Physical entry point of the loaded module (phys_load_handoff).
    [[nodiscard]]
    auto entry() const -> PhysAddr {
        return m_entry;
    }

    /// The materialized kernel-package BootFS range handed to the module.
    [[nodiscard]]
    auto storage_base() const -> PhysAddr {
        return m_storage_base;
    }

    [[nodiscard]]
    auto storage_size() const -> usize {
        return m_storage_size;
    }

  private:
    ElfImage m_image;
    PhysAddr m_entry{0};
    PhysAddr m_storage_base{0};
    usize m_storage_size{0};
};

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_MODULE_LOADER_HPP
