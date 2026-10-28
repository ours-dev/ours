/// PhysLoad — first-stage boot environment initializer.
///
/// physload is the first program the bootloader runs.  It:
///   1. Parses the outer OBI package and validates the boot contract
///   2. Applies early boot options (KASLR, load target)
///   3. Initializes physical memory (BootMem) and the temporary address space
///   4. [Phase B] Decompresses KERNEL_STORAGE and loads the next phys module
///   5. Calls the entry the module declared through PHYS_MODULE()
///
/// physload materializes the kernel package, loads the next phys module
/// (`kernel.phys.next`, physboot by default) as a separate image, relocates
/// it, binds its undefined symbols to physload's own definitions, and calls
/// the entry named by the module's descriptor.

#include "physload.hpp"

#include <ours/phys/aspace.hpp>
#include <ours/phys/boot_obi.hpp>
#include <ours/phys/boot_options.hpp>
#include <ours/phys/boot_package_input.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/memory_provisioner.hpp>
#include <ours/phys/module_loader.hpp>

#include <span>

#include <ccrt/runtime.hpp>
#include <logz4/log.hpp>
#include <obi/command_line_view.hpp>
#include <obi/container_view.hpp>

namespace ours::phys {
Handoff HANDOFF;
BootOptions BOOT_OPTIONS;

/// Default: nothing to do between loading a module and entering it.  An
/// architecture overrides this to install a page-table root or reserve
/// arch-specific memory (physload.hpp).
WEAK_LINK
auto arch_on_phys_load_handoff(PhysLoadHandoff const &handoff) -> void {}

[[noreturn]]
auto obi_main(PhysAddr package_base, usize package_size, LegacyPlatformInfo const *legacy) -> void {
    // Static console instances self-register during this pass (each
    // Console constructor links into the constinit global list); printing
    // before this point goes nowhere.
    ccrt::init_global_static_objects();
    init_early_console();
    HANDOFF.uart = arch_uart_handoff();

    if (legacy != nullptr) {
        HANDOFF.acpi_rsdp = legacy->acpi_rsdp;
        HANDOFF.device_tree = legacy->device_tree;
        if (!legacy->bootloader_name.empty()) {
            log::info("physload: loaded by '{}'", legacy->bootloader_name);
        }
    }

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
    auto boot_package_result = BootObi::from(package);
    if (boot_package_result.is_err()) {
        panic("Invalid OBI boot package: {}", static_cast<u32>(boot_package_result.unwrap_err()));
    }
    auto const boot = boot_package_result.unwrap();

    // ---- Stage 2: Apply early boot options ----
    //
    // Two sources, applied in precedence order.  The container's own Config
    // item is what the build baked in; the legacy command line is what the
    // user typed in the boot menu this boot, so it goes second and wins on
    // any option both spell.  BootOptions::apply() only overwrites the
    // options a command line actually names, which is what makes layering
    // them work.
    if (auto const command_line = boot.command_line(); command_line.is_some()) {
        auto const options_status = BOOT_OPTIONS.apply(command_line.value());
        if (options_status != Status::Ok) {
            panic("Invalid OBI boot options: {}", static_cast<u32>(options_status));
        }
    }
    if (legacy != nullptr && !legacy->cmdline.empty()) {
        auto const command_line = obi::CommandLineView::from_text(legacy->cmdline);
        if (command_line.is_err()) {
            panic("Malformed bootloader command line: {}",
                  static_cast<u32>(command_line.unwrap_err()));
        }
        auto const options_status = BOOT_OPTIONS.apply(command_line.unwrap());
        if (options_status != Status::Ok) {
            panic("Invalid bootloader boot options: {}", static_cast<u32>(options_status));
        }
    }

    // ---- Stage 3: Initialize memory and address space ----
    Aspace aspace;
    std::span<obi::MemoryRange const> const legacy_memory_ranges =
        legacy == nullptr ? std::span<obi::MemoryRange const>{}
                          : std::span(legacy->memory_ranges, legacy->memory_range_count);
    MemoryProvisioner memory{*global_bootmem()};
    memory.detect(boot.package(), legacy_memory_ranges);
    memory.reserve_boot_image();
    memory.reserve_package(input.unwrap());
    memory.reserve_crashlog(BOOT_OPTIONS);
    memory.install(&aspace);

    // ---- Stage 4: Load the next phys module ----
    //
    // a) materialize the kernel package (the single StorageKernel item)
    //    into bootmem-owned scratch
    // b) find the module named by BOOT_OPTIONS.next_module
    //    (`kernel.phys.next`, "physboot" by default) in the BootFS
    // c) load it as an ET_DYN image and apply its relative relocations
    ModuleLoader loader;
    auto const storage_status = loader.init(boot);
    if (storage_status != Status::Ok) {
        panic("Failed to materialize kernel package: {}", static_cast<u32>(storage_status));
    }
    auto const load_status = loader.load(BOOT_OPTIONS);
    if (load_status != Status::Ok) {
        panic("Failed to load phys module: {}", static_cast<u32>(load_status));
    }

    PhysLoadHandoff phys_handoff{};
    phys_handoff.bootmem = global_bootmem();
    phys_handoff.aspace = &aspace;
    phys_handoff.kernel_storage_base = loader.storage_base();
    phys_handoff.kernel_storage_size = loader.storage_size();
    phys_handoff.obi_package_base = package_base;
    phys_handoff.obi_package_size = package_size;
    phys_handoff.boot_options = &BOOT_OPTIONS;
    phys_handoff.acpi_rsdp = HANDOFF.acpi_rsdp;

    // ---- Stage 5: Hand off to the next phys module ----
    //
    // The module declared its own entry through PHYS_MODULE(); start()
    // runs the module's constructors and calls it.  Never returns: the
    // module either boots the kernel or panics.
    loader.start(phys_handoff);
}

} // namespace ours::phys
