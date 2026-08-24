/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_MODULE_LOADER_HPP
#define OURS_PHYS_MODULE_LOADER_HPP 1

#include <ours/phys/boot-obi.hpp>
#include <ours/phys/boot-options.hpp>
#include <ours/phys/kernel-image.hpp>
#include <ours/compiler.hpp>
#include <ours/phys/module.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::phys {

/// Loads the next phys module out of the kernel package — the loader half
/// of the physload→physboot split (the object a `physload_module()` GN
/// target produces).
///
/// init() materializes the kernel package (the single StorageKernel item)
/// into a bootmem-owned scratch buffer.  load() locates the module named by
/// `kernel.phys.next` in the resulting BootFS (falling back to "physboot"),
/// loads it, relocates it, binds its imports against physload's export
/// table, and reads the PHYS_MODULE() descriptor out of its `.phys.module`
/// section.  start() then runs the module's constructors and calls the
/// entry that descriptor names.
///
/// There is no fixed entry symbol and no ELF entry address in any of this:
/// what to call is data the module carries, which is what lets physload
/// check the module's ABI version before jumping into it.
class ModuleLoader final {
  public:
    /// Decompress the kernel package into bootmem scratch memory.  The
    /// buffer's ownership moves to the loaded module through
    /// PhysLoadHandoff::kernel_storage_base/size and is never released.
    auto init(BootObi const &boot) -> Status;

    /// Find, load, relocate and bind the module named by
    /// `options.next_module`, and read its descriptor.
    auto load(BootOptions const &options) -> Status;

    /// Run the loaded module's static constructors and call its entry.
    /// Never returns -- the module either boots something or panics.
    [[noreturn]]
    auto start(PhysLoadHandoff const &handoff) -> void;

    /// The descriptor the loaded module declared about itself.
    ///
    /// Spelled `descriptor`, not `module`: since C++20 `module` is an
    /// identifier with special meaning, and a line that starts with it can
    /// be taken for a module-directive.
    [[nodiscard]]
    auto descriptor() const -> PhysModule const * {
        return m_descriptor;
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
    /// Run the loaded image's .init_array in place.
    auto run_initializers() -> void;

    ElfImage m_image;
    PhysModule const *m_descriptor{nullptr};
    PhysAddr m_storage_base{0};
    usize m_storage_size{0};
};

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_MODULE_LOADER_HPP
