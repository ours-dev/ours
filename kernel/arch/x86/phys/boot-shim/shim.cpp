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
/// (ours/phys/legacy-platform-info.hpp).  Genuinely appending them as OBI
/// items instead is deferred; see
/// .agents/tasks/phys-module-and-obi-tree/DESIGN.md.

#include <ours/panic.hpp>
#include <ours/phys/boot-obi.hpp>
#include <ours/phys/console.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/kernel-image.hpp>
#include <ours/phys/legacy-platform-info.hpp>
#include <ours/phys/print.hpp>

#include <multiboot/view.hpp>
#include <obi/container-view.hpp>

#include "../legacy-boot.hpp"

#include <ccrt/runtime.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/option.hpp>

#include <algorithm>
#include <array>

namespace ours::phys {
namespace {
    extern "C" char const __executable_start[];
    extern "C" char const __executable_end[];

    using MemoryMap = multiboot::View::MemoryMap;

    /// boot-shim never expects more than a handful of firmware-reported
    /// ranges; if a platform ever hands it more, better to say so loudly
    /// (missing ranges silently become "not usable") than to allocate for
    /// an unbounded count this early.
    constexpr usize kMaxLegacyMemoryRanges = 64;

    [[nodiscard]]
    constexpr auto to_obi_memory_type(u32 const multiboot_type) noexcept -> obi::MemoryRangeType {
        return multiboot_type == multiboot::MultibootMmapEntry::Available ? obi::MemoryRangeType::Ram
                                                                : obi::MemoryRangeType::Reserved;
    }

    /// Convert legacy.memory_map into the same obi::MemoryRange vocabulary
    /// a real OBI memory item already carries.  Returns the number of
    /// ranges written into `out` (which may be fewer than the source map
    /// has, per kMaxLegacyMemoryRanges).
    [[nodiscard]]
    auto convert_memory_map(MemoryMap const &map,
                            std::array<obi::MemoryRange, kMaxLegacyMemoryRanges> &out) -> usize {
        auto const count = std::min(map.count(), kMaxLegacyMemoryRanges);
        if (map.count() > kMaxLegacyMemoryRanges) {
            println("boot-shim: memory map has {} ranges, only converting the first {}",
                    map.count(), kMaxLegacyMemoryRanges);
        }
        for (usize index = 0; index < count; ++index) {
            auto const entry = map.range(index);
            out[index] = obi::MemoryRange{
                .base = entry.addr,
                .length = entry.len,
                .type = static_cast<u32>(to_obi_memory_type(entry.type)),
                .reserved = 0,
            };
        }
        return count;
    }

    /// Find a page-aligned range of `size` Available bytes at or after
    /// `floor`.  A bump allocator over the platform's own memory map, not
    /// this image's own: physload does not exist yet for the shim to ask
    /// BootMem, and nothing upstream of the shim owns memory management
    /// either.  `floor` is the caller's job -- this function only
    /// guarantees the returned range starts at or after it, not that
    /// nothing else the caller cares about sits inside it.
    [[nodiscard]]
    auto find_scratch(MemoryMap const &map, PhysAddr const floor, usize const size)
        -> ustl::Option<PhysAddr> {
        for (usize index = 0; index < map.count(); ++index) {
            auto const region = map.range(index);
            if (region.type != multiboot::MultibootMmapEntry::Available || region.len < size) {
                continue;
            }
            if (region.addr > std::numeric_limits<u64>::max() - region.len) {
                continue; // Malformed entry; already rejected by MemoryMap::from in
                          // principle, but this function does not get to assume that.
            }
            auto const region_end = region.addr + region.len;
            auto const candidate = ustl::mm::align_up(
                std::max(region.addr, static_cast<u64>(floor)), static_cast<u64>(PAGE_SIZE));
            if (candidate < region.addr || candidate >= region_end ||
                region_end - candidate < size) {
                continue;
            }
            return ustl::some(static_cast<PhysAddr>(candidate));
        }
        return ustl::NONE;
    }

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
        auto const init_status =
            image.init({physload_bytes.data(), physload_bytes.size()}, physload_bytes.size());
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
        auto const floor =
            std::max({PhysAddr(__executable_end),
                      PhysAddr(legacy.ramdisk.data()) + legacy.ramdisk.size(),
                      mbi_base + legacy.mbi_size});
        auto const total = ustl::mm::align_up(image.size(), PAGE_SIZE);
        auto const target = find_scratch(legacy.memory_map.value(), floor, total);
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

        println("boot-shim: physload at 0x{:X}, entry 0x{:X}", target.value(), entry.unwrap());

        // Stack-local, not scratch-allocated: physload and everything it
        // loads in turn (physboot, the kernel) run as plain nested calls on
        // this same stack -- nothing downstream switches stacks before the
        // kernel does -- so this array outlives every read obi_main() or
        // arch_detect_memory() will ever make of it.
        std::array<obi::MemoryRange, kMaxLegacyMemoryRanges> ranges{};
        auto const range_count = convert_memory_map(legacy.memory_map.value(), ranges);
        LegacyPlatformInfo const info{
            .acpi_rsdp = legacy.acpi_rsdp != 0 ? ustl::some(PhysAddr(legacy.acpi_rsdp)) : ustl::NONE,
            .memory_ranges = ranges.data(),
            .memory_range_count = range_count,
        };

        auto const module_entry =
            reinterpret_cast<void (*)(PhysAddr, usize, LegacyPlatformInfo const *)>(
                entry.unwrap());
        module_entry(PhysAddr(module.data()), module.size(), &info);

        panic("Never reach at here");
    }
} // namespace

NO_MANGLE NO_RETURN
auto phys_main(PhysAddr loader_param) -> void {
    ccrt::init_global_static_objects();
    init_early_console();
    println("boot-shim: loader params at 0x{:X}", loader_param);

    auto &legacy = LegacyBoot::get();
    auto const handoff_status = legacy.parse_params(loader_param);
    if (handoff_status != Status::Ok) {
        panic("Invalid Multiboot2 handoff: {}", static_cast<u32>(handoff_status));
    }

    load_physload(legacy, loader_param);
}

} // namespace ours::phys
