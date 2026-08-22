/// PhysLoad — first-stage boot environment initializer.
///
/// physload is the first program the bootloader runs.  It:
///   1. Parses the outer OBI package and validates the boot contract
///   2. Applies early boot options (KASLR, load target)
///   3. Initializes physical memory (BootMem) and the temporary address space
///   4. [Phase B] Decompresses KERNEL_STORAGE and loads the next phys module
///   5. Hands off to physload-module (the standard adapter) which calls
///      the module's own phys_load_module_main.
///
/// physload materializes the kernel package, loads the next phys module
/// (`kernel.phys.next`, physboot by default) as a separate ET_DYN ELF,
/// relocates it, and tail-calls its phys_load_handoff entry.

#include <ccrt/runtime.hpp>
#include <obi/boot-obi.hpp>
#include <obi/container-view.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/boot-options.hpp>
#include <ours/phys/boot-package-input.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/module-loader.hpp>
#include <ours/phys/physload.hpp>
#include <ours/phys/print.hpp>

namespace ours::phys {
Handoff g_handoff;
BootOptions g_boot_options;

NO_RETURN
auto obi_main(PhysAddr package_base, usize package_size) -> void {
    // Static console instances self-register during this pass (each
    // Console constructor links into the constinit global list); printing
    // before this point goes nowhere.
    ccrt::init_global_static_objects();
    init_early_console();

    // ---- Stage 1: Validate the OBI boot package ----
    auto const input = BootPackageInput::from(package_base, package_size);
    if (input.is_err()) {
        panic("Invalid OBI package range: {}", static_cast<u32>(input.unwrap_err()));
    }
    auto const bytes = input.unwrap().bytes();
    auto package_result = obi::ContainerView::from(bytes.data(), bytes.size());
    if (package_result.is_err()) {
        panic("Invalid OBI package: {}", static_cast<u32>(package_result.unwrap_err()));
    }
    auto package = package_result.unwrap();
    auto boot_package_result = obi::BootObi::from(package);
    if (boot_package_result.is_err()) {
        panic("Invalid OBI boot package: {}", static_cast<u32>(boot_package_result.unwrap_err()));
    }
    auto const boot = boot_package_result.unwrap();

    // ---- Stage 2: Apply early boot options ----
    if (auto const command_line = boot.command_line(); command_line.is_some()) {
        auto const options_status = apply_boot_options(g_boot_options, command_line.value());
        if (options_status != Status::Ok) {
            panic("Invalid OBI boot options: {}", static_cast<u32>(options_status));
        }
    }

    // ---- Stage 3: Initialize memory and address space ----
    Aspace aspace;
    init_memory(boot.package(), input.unwrap(), &aspace);

    // ---- Stage 4: Load the next phys module ----
    //
    // a) materialize the kernel package (the single StorageKernel item)
    //    into bootmem-owned scratch
    // b) find the module named by g_boot_options.next_module
    //    (`kernel.phys.next`, "physboot" by default) in the BootFS
    // c) load it as an ET_DYN image and apply its relative relocations
    ModuleLoader loader;
    auto const storage_status = loader.init(boot);
    if (storage_status != Status::Ok) {
        panic("Failed to materialize kernel package: {}", static_cast<u32>(storage_status));
    }
    auto const load_status = loader.load(g_boot_options);
    if (load_status != Status::Ok) {
        panic("Failed to load phys module: {}", static_cast<u32>(load_status));
    }

    PhysLoadHandoff phys_handoff{};
    phys_handoff.bootmem = early_mem();
    phys_handoff.aspace = &aspace;
    phys_handoff.kernel_storage_base = loader.storage_base();
    phys_handoff.kernel_storage_size = loader.storage_size();
    phys_handoff.obi_package_base = package_base;
    phys_handoff.obi_package_size = package_size;
    phys_handoff.boot_options = &g_boot_options;
    phys_handoff.acpi_rsdp = g_handoff.acpi_rsdp;
    phys_handoff.nr_cpus = g_handoff.nr_cpus;

    // ---- Stage 5: Hand off to the next phys module ----
    //
    // Tail-call the module's entry (phys_load_handoff — the standard
    // adapter that installs module services and calls the module's
    // phys_load_module_main).  Neither returns: the module either boots
    // the kernel or panics.
    auto const module_entry = reinterpret_cast<PhysLoadHandoffFunction>(loader.entry());
    module_entry(phys_handoff);

    panic("Never reach at here");
}

} // namespace ours::phys
