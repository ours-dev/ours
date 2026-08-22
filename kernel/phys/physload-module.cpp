/// physload-module — standard adapter between physload and phys modules
///
/// This file is linked into every phys module (physboot, netboot, ...).
/// It provides phys_load_handoff, the single entry point physload
/// tail-calls after loading and relocating the module ELF.
///
/// There is nothing to mirror here: physload carries the phys runtime, and
/// ModuleLoader has already bound this image's references to g_handoff,
/// g_boot_options, g_aspace and the rest straight to physload's definitions
/// (ours/phys/exports.hpp).  The adapter only has to bring the module's own
/// image to life and hand control on.

#include <ccrt/runtime.hpp>
#include <ours/phys/boot-options.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/physboot.hpp>
#include <ours/phys/physload.hpp>
#include <ours/phys/print.hpp>

namespace ours::phys {

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
    // Run this image's own static constructors.  Each ELF has its own
    // .ccrt_init array, so this covers the module's statics only —
    // physload's ran in physload's image, and the console list, BootMem and
    // Aspace they built are the ones the module has just been bound to.
    ccrt::init_global_static_objects();

    // 1. Architecture-specific post-handoff initialization.
    arch_on_phys_load_handoff(handoff);

    // 2. Call the module's own entry point.  This function is NO_RETURN —
    //    control never returns here.
    phys_load_module_main(handoff);
}

} // namespace ours::phys
