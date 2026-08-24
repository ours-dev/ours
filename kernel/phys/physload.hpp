/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_PHYS_PHYSLOAD_HPP
#define OURS_PHYS_PHYSLOAD_HPP 1

#include <ours/compiler.hpp>
#include <ours/phys/boot-options.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/types.hpp>

namespace ours::phys {

class PHYS_EXPORT Aspace;

/// PhysLoadHandoff carries all state from physload (the first-stage loader)
/// to the next phys module (e.g. physboot), via physload-module's standard
/// adapter.
///
/// physload fills it in and passes it to the entry the module declared
/// through PHYS_MODULE() (ours/phys/module.hpp).  Everything else the module
/// needs -- the console, the allocator, the address space -- it reaches by
/// binding to physload's definitions, so this carries only what is specific
/// to one load.
struct PhysLoadHandoff {
    /// The global BootMem instance -- a live pointer, not a copy: the
    /// module keeps allocating through it (its own scratch buffers, page
    /// tables when relocating the kernel image) until it hands off to
    /// vmours in turn.
    BootMem *bootmem;

    /// Address space established by physload (page table root, identity
    /// mappings).  May be nullptr when the module is expected to build its
    /// own page tables from scratch.
    Aspace *aspace;

    /// Physical range of the decompressed kernel-storage payload (bootfs
    /// containing phys modules and the nested kernel-package).  In Phase A
    /// this is the kernel-package payload directly from the OBI outer
    /// package; in Phase B it is the decompressed KSTR item.
    PhysAddr kernel_storage_base;
    usize kernel_storage_size;

    /// Physical range of the complete outer OBI package, retained so later
    /// stages can re-parse it.
    PhysAddr obi_package_base;
    usize obi_package_size;

    /// Parsed boot options (KASLR, load target, etc.).
    BootOptions *boot_options;

    /// ACPI RSDP physical address discovered by the boot shim.
    ustl::Option<PhysAddr> acpi_rsdp;
};

/// Architecture-specific hook run just before a loaded module's entry.
///
/// This lives in physload, not in the module: the module has no startup
/// code of its own to hang it off any more.  The default is a no-op; an
/// architecture overrides it (via WEAK_LINK) for post-load setup such as
/// installing a page-table root or reserving arch-specific memory.
auto arch_on_phys_load_handoff(PhysLoadHandoff const &handoff) -> void;

} // namespace ours::phys

#endif // OURS_PHYS_PHYSLOAD_HPP
