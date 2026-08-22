/// physload-module — standard adapter between physload and phys modules
///
/// This file is linked into every phys module (physboot, netboot, ...).
/// It provides phys_load_handoff, the single entry point physload
/// tail-calls after loading and relocating the module ELF: it mirrors the
/// loader-side globals the module's code reaches through the init.hpp
/// accessors, then calls the module's own phys_load_module_main.

#include <ccrt/runtime.hpp>
#include <ours/phys/boot-options.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/physboot.hpp>
#include <ours/phys/physload.hpp>
#include <ours/phys/print.hpp>

namespace ours::phys {

/// Module-side storage for the globals that physload owns in its own ELF.
/// Filled from the PhysLoadHandoff below, before any module code runs.
Handoff g_handoff;
BootOptions g_boot_options;
Aspace *g_aspace = nullptr;

/// Architecture-specific hook called after the handoff is received
/// and before phys_load_module_main.  Default is a no-op; each architecture
/// overrides this to perform post-handoff setup (e.g. install a new page
/// table root, reserve arch-specific memory regions).
WEAK_LINK
auto arch_on_phys_load_handoff(PhysLoadHandoff const &handoff) -> void {}

/// Entry point called by physload.  The module's link script makes this
/// the ELF's e_entry, so physload needs no symbol lookup.  This function
/// MUST have C linkage so the entry resolves reliably across toolchains.
NO_MANGLE NO_RETURN
auto phys_load_handoff(PhysLoadHandoff const &handoff) -> void {
    // The module's own console instances register during this pass and
    // activate below (serial is re-initialized; VGA is cleared).  Output
    // before this point went through the loader's consoles.
    ccrt::init_global_static_objects();
    init_early_console();

    // Mirror the loader-side globals for the module's lifetime.
    g_boot_options = *handoff.boot_options;
    g_aspace = handoff.aspace;
    g_handoff.acpi_rsdp = handoff.acpi_rsdp;
    g_handoff.nr_cpus = handoff.nr_cpus;

    // 1. Architecture-specific post-handoff initialization.
    arch_on_phys_load_handoff(handoff);

    // 2. Call the module's own entry point.  This function is NO_RETURN —
    //    control never returns here.
    phys_load_module_main(handoff);
}

} // namespace ours::phys
