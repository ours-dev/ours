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

class Aspace;

/// PhysLoadHandoff carries all state from physload (the first-stage loader)
/// to the next phys module (e.g. physboot), via physload-module's standard
/// adapter.
///
/// In Phase A (same-ELF modularization) this is passed by const-ref through
/// a direct function call.  In Phase B (multi-ELF) physload will fill it and
/// pass it to the relocated module through phys_load_handoff().
struct PhysLoadHandoff {
    /// Type-erased bootmem allocator: reserve / assign_node / allocate /
    /// release / visit callbacks backed by the global BootMem instance.
    EarlyMem bootmem;

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
    PhysAddr acpi_rsdp;

    /// Number of CPUs reported by the boot loader / firmware.
    u32 nr_cpus;
};

/// Entry-point signature for a physload-loaded module.  physload relocates
/// the module ELF, verifies its build ID, and then calls this function with
/// the populated handoff.  The implementation must be NO_MANGLE (extern "C")
/// so that physload can locate it by symbol name.
///
/// Implemented by physload-module (the standard adapter library linked into
/// every phys module).
using PhysLoadHandoffFunction = auto (*)(PhysLoadHandoff const &handoff) -> void;

/// Standard adapter entry point, provided by physload-module.
///
/// physload calls this after initializing the boot environment.  It installs
/// standard services (console, allocation pool, symbolize), calls
/// arch_on_phys_load_handoff(), and then delegates to phys_load_module_main().
/// This function is NO_RETURN — it either boots the kernel or panics.
NO_MANGLE NO_RETURN auto phys_load_handoff(PhysLoadHandoff const &handoff) -> void;

/// Architecture-specific hook invoked by phys_load_handoff() after the
/// handoff is received and before phys_load_module_main().
///
/// The default implementation is a no-op.  Each architecture overrides it
/// (via WEAK_LINK) to perform post-handoff setup such as installing a new
/// page table root or reserving arch-specific memory regions.
auto arch_on_phys_load_handoff(PhysLoadHandoff const &handoff) -> void;

} // namespace ours::phys

#endif // OURS_PHYS_PHYSLOAD_HPP
