/// x86 boot-shim — GRUB's Multiboot2 entry point.
///
/// This is where the split the rest of the tree assumes actually happens:
/// downstream of here, nothing understands Multiboot2, only OBI.  The shim
/// parses the Multiboot2 handoff, views the OBI container GRUB loaded as a
/// module, loads physload (the container's item 0) out of it as a separate
/// image, and jumps to physload's own entry point.
///
/// physload is compiled position-independent and placed wherever this
/// finds room -- ElfImage::init/load_at/relocate are already proven by
/// physload's own use of them to load physboot; this is the same
/// mechanism one hop earlier.  There is no symbol binding and no build-id
/// pairing on this hop, unlike physload<->physboot: physload's entry takes
/// a handful of plain values (obi_main(PhysAddr, usize,
/// LegacyPlatformInfo const*) -- no methods, no vtables, and boot-shim and
/// physload are always built together from the same tree, unlike a
/// physload_module()), so no fragile ABI crosses this boundary.
///
/// ACPI RSDP and the memory map cross into physload as that third
/// argument, converted from Multiboot2's own vocabulary into the same
/// obi::MemoryRange shape a real OBI memory item already uses
/// (ours/phys/legacy_platform_info.hpp).  Genuinely appending them as OBI
/// items instead is deferred; see
/// .agents/tasks/phys-module-and-obi-tree/DESIGN.md.

#include <ours/panic.hpp>
#include <ours/phys/boot_obi.hpp>
#include <ours/phys/console.hpp>
#include <ours/phys/elf_image.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/legacy_platform_info.hpp>

#include <algorithm>
#include <array>

#include "../legacy_boot.hpp"
#include <bootshim/legacy_memory.hpp>
#include <ccrt/runtime.hpp>
#include <logz4/log.hpp>
#include <multiboot/view.hpp>
#include <obi/container_view.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/option.hpp>

namespace ours::phys {
namespace {
    extern "C" char const __executable_start[];
    extern "C" char const __executable_end[];

    using bootshim::LegacyMemoryTranslator;
    using bootshim::MAX_LEGACY_MEMORY_RANGES;

    /// Load physload out of the OBI container's item 0 and jump to it.
    /// Never returns.  `mbi_base` is the raw Multiboot2 info block address
    /// phys_main was handed -- not reachable from `legacy` itself, whose
    /// memory_map view holds a pointer into that block's bytes but not the
    /// block's own base address.
    [[noreturn]]
    auto load_physload(LegacyBoot const &legacy, PhysAddr const mbi_base) -> void {
        auto const module = legacy.ramdisk;
        auto const container_result = obi::ContainerView::from(module.data(), module.size());
        if (container_result.is_err()) {
            panic("Invalid OBI container: {}", static_cast<u32>(container_result.unwrap_err()));
        }
        auto const boot_result = BootObi::from(container_result.unwrap());
        if (boot_result.is_err()) {
            panic("Invalid OBI boot package: {}", static_cast<u32>(boot_result.unwrap_err()));
        }
        auto const item0 = boot_result.unwrap().executable();
        if (item0.is_none()) {
            panic("OBI container has no item 0 executable (physload)");
        }
        auto const physload_bytes = item0.value().payload();

        if (legacy.memory_map.is_none()) {
            panic("No Multiboot2 memory map; cannot place physload");
        }

        ElfImage image;
        auto const init_status = image.init({physload_bytes.data(), physload_bytes.size()});
        if (init_status != Status::Ok) {
            panic("Invalid physload ELF: {}", static_cast<u32>(init_status));
        }

        // Keep off: this image (shim itself, still running), the Multiboot
        // info block (legacy.memory_map is a *view* into it -- corrupting
        // it here would corrupt the very map find_scratch is about to
        // read), and the OBI module (physload_bytes is a view into it, and
        // ElfImage::load_at is about to read from it while writing
        // somewhere else -- letting the two ranges overlap could have it
        // read its own half-written output).
        auto const floor = std::max({PhysAddr(__executable_end),
                                     PhysAddr(legacy.ramdisk.data()) + legacy.ramdisk.size(),
                                     mbi_base + legacy.mbi_size});
        auto const total = ustl::mm::align_up(image.size(), PAGE_SIZE);
        auto const target = bootshim::LegacyMemoryTranslator::find_scratch(
            legacy.memory_map.value(), floor, total, PAGE_SIZE);
        if (target.is_none()) {
            panic("No memory to place physload ({} bytes wanted)", total);
        }

        auto const entry = image.load_at(target.value());
        if (entry.is_err()) {
            panic("Failed to load physload: {}", static_cast<u32>(entry.unwrap_err()));
        }
        if (auto const status = image.relocate(target.value()); status != Status::Ok) {
            panic("Failed to relocate physload: {}", static_cast<u32>(status));
        }

        log::info("boot-shim: physload at 0x{:X}, entry 0x{:X}", target.value(), entry.unwrap());

        // Stack-local, not scratch-allocated: physload and everything it
        // loads in turn (physboot, the kernel) run as plain nested calls on
        // this same stack -- nothing downstream switches stacks before the
        // kernel does -- so the translator and the ranges it owns outlive
        // every read obi_main() or arch_detect_memory() will ever make of
        // them.
        LegacyMemoryTranslator translator;
        auto const converted = translator.convert(legacy.memory_map.value());
        if (converted.dropped != 0) {
            // Loud on purpose: a range that never reaches physload silently
            // becomes "not usable", so an operator wondering where their RAM
            // went has to be able to see this happen.
            log::info("boot-shim: memory map has {} ranges, only converting the first {}",
                      converted.converted + converted.dropped, MAX_LEGACY_MEMORY_RANGES);
        }
        // cmdline/name point into the Multiboot info block, which the floor
        // above already keeps physload off of, so they stay readable for as
        // long as obi_main() cares to read them.
        LegacyPlatformInfo const info{
            .acpi_rsdp =
                legacy.acpi_rsdp != 0 ? ustl::some(PhysAddr(legacy.acpi_rsdp)) : ustl::NONE,
            .memory_ranges = translator.ranges().data(),
            .memory_range_count = converted.converted,
            .cmdline = legacy.cmdline,
            .bootloader_name = legacy.name,
        };

        auto const module_entry =
            reinterpret_cast<void (*)(PhysAddr, usize, LegacyPlatformInfo const *)>(entry.unwrap());
        module_entry(PhysAddr(module.data()), module.size(), &info);

        panic("Never reach at here");
    }
} // namespace

extern "C" [[noreturn]]
auto phys_main(PhysAddr loader_param) -> void {
    ccrt::init_global_static_objects();
    init_early_console();
    log::info("boot-shim: loader params at 0x{:X}", loader_param);

    auto &legacy = LegacyBoot::get();
    auto const handoff_status = legacy.parse_params(loader_param);
    if (handoff_status != Status::Ok) {
        panic("Invalid Multiboot2 handoff: {}", static_cast<u32>(handoff_status));
    }

    load_physload(legacy, loader_param);
}

} // namespace ours::phys
