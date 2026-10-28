/// riscv boot-shim.
///
/// The firmware (OpenSBI) enters the image through `_start` (start.S) with the
/// boot hart's ID and the device tree's physical address. The shim reads the
/// tree, turns it into the memory map physload is handed
/// (bootshim::DevicetreeTranslator), reports that map, the initrd and the
/// command line over the firmware's console, and fills the LegacyPlatformInfo
/// physload's obi_main() takes. Loading physload out of the initrd and calling
/// it is the next step; until then the shim parks the hart.
///
/// A tree the shim cannot read is refused with the reason, and the hart
/// parked: nothing downstream could do better with a map it could not trust.

#include <ours/compiler.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/phys/legacy_platform_info.hpp>
#include <ours/prelude.hpp>
#include <ours/status.hpp>

#include <array>
#include <span>
#include <string_view>

#include <arch/riscv/sbi.hpp>
#include <bootshim/devicetree.hpp>
#include <fdt/tree.hpp>
#include <obi/package_format.hpp>

/// This image's bounds, from phys.ld: its first byte, and the end of .bss,
/// which holds the boot stack.
extern char const IMAGE_START[] LINK_NAME("__executable_start");
extern char const IMAGE_END[] LINK_NAME("__executable_end");

namespace {
using ours::PhysAddr;
using ours::Status;

/// The console is the firmware's, one byte per call, so printing is a loop.
/// There is no formatting runtime this early.
auto print(std::string_view const text) -> void {
    for (auto const ch: text) {
        arch::sbi::console_putchar(ch);
    }
}

/// `value` in hex, without leading zeros.
auto print_hex(u64 const value) -> void {
    constexpr std::string_view DIGITS = "0123456789abcdef";
    constexpr u64 NIBBLE_BITS = 4;

    print("0x");
    auto shift = u64{60};
    while (shift > 0 && (value >> shift) == 0) {
        shift -= NIBBLE_BITS;
    }
    while (true) {
        arch::sbi::console_putchar(DIGITS[(value >> shift) & 0xf]);
        if (shift == 0) {
            break;
        }
        shift -= NIBBLE_BITS;
    }
}

auto print_decimal(u64 const value) -> void {
    std::array<char, 20> digits{};
    auto count = usize{0};
    auto rest = value;
    do {
        digits[count++] = static_cast<char>('0' + (rest % 10));
        rest /= 10;
    } while (rest != 0);
    while (count > 0) {
        arch::sbi::console_putchar(digits[--count]);
    }
}

/// `[base, end)`, half-open like every range in the map.
auto print_range(u64 const base, u64 const size) -> void {
    print("[");
    print_hex(base);
    print(", ");
    print_hex(base + size);
    print(")");
}

auto type_name(u32 const type) -> std::string_view {
    switch (static_cast<obi::MemoryRangeType>(type)) {
        case obi::MemoryRangeType::Ram:
            return "ram";
        case obi::MemoryRangeType::Peripheral:
            return "peripheral";
        case obi::MemoryRangeType::Reserved:
            return "reserved";
    }
    return "unknown";
}

/// What a refused conversion means, in the words of
/// DevicetreeTranslator::convert()'s contract.
auto conversion_failure(Status const status) -> std::string_view {
    switch (status) {
        case Status::InvArgs:
            return "a property is not in the shape its binding gives it";
        case Status::NotSupported:
            return "an address or size is wider than two cells";
        case Status::OutOfRange:
            return "a range runs past the top of the address space";
        case Status::NotFound:
            return "it describes no RAM";
        case Status::AlreadyExists:
            return "the initrd overlaps memory the firmware or this image holds";
        default:
            return "unexpected status";
    }
}

/// wfi returns on any pending interrupt, which are all masked, so this
/// sleeps; the loop is for a spurious wake.
[[noreturn]]
auto park() -> void {
    while (true) {
        asm volatile("wfi" ::: "memory");
    }
}

[[noreturn]]
auto refuse(std::string_view const what, Status const status) -> void {
    print("[riscv-bootshim] ");
    print(what);
    print(" (");
    print(ours::to_string(status));
    print("); halting\n");
    park();
}

/// Everything physload will be handed, as it will read it: the platform info,
/// and the initrd, where physload's OBI package is.
auto report(ours::phys::LegacyPlatformInfo const &info,
            ustl::Option<bootshim::PhysRange> const initrd) -> void {
    auto const ranges = std::span(info.memory_ranges, info.memory_range_count);
    print("[riscv-bootshim] memory map, ");
    print_decimal(ranges.size());
    print(" ranges:\n");
    for (auto const &range: ranges) {
        print("[riscv-bootshim]   ");
        print_range(range.base, range.length);
        print(" ");
        print(type_name(range.type));
        print("\n");
    }

    if (initrd.is_some()) {
        print("[riscv-bootshim] initrd ");
        print_range(initrd.value().base, initrd.value().size);
        print(", ");
        print_decimal(initrd.value().size);
        print(" bytes\n");
    } else {
        print("[riscv-bootshim] no initrd\n");
    }

    if (info.cmdline.empty()) {
        print("[riscv-bootshim] command line: none\n");
    } else {
        print("[riscv-bootshim] command line: \"");
        print(info.cmdline);
        print("\"\n");
    }
}

} // namespace

/// Called by start.S with a0 and a1 as the firmware left them.
extern "C" [[noreturn]]
auto bootshim_main(usize const hart_id, usize const device_tree) -> void {
    print("[riscv-bootshim] hart ");
    print_hex(hart_id);
    print(" dtb ");
    print_hex(device_tree);
    print("\n");

    if (device_tree == 0) {
        refuse("no device tree: a1 is 0", Status::InvArgs);
    }

    // The header says how long the blob is; Tree::from() checks the magic
    // before it believes that, and reads nothing past it.
    auto const *const blob = reinterpret_cast<u8 const *>(device_tree);
    auto const parsed = fdt::Tree::from(blob, fdt::load_be32(blob + fdt::header::TOTAL_SIZE));
    if (parsed.is_err()) {
        refuse("the device tree is malformed", parsed.unwrap_err());
    }
    auto const &tree = parsed.unwrap();

    // Kept out of the allocator's hands until the kernel is done with them:
    // this image, where the map and the platform info live while physload
    // runs, and the tree, which the kernel walks itself.
    auto const image = PhysAddr(IMAGE_START);
    std::array<bootshim::PhysRange, 2> const own{{
        {.base = image, .size = PhysAddr(IMAGE_END) - image},
        {.base = device_tree, .size = tree.blob().size()},
    }};

    // Stack-local, as on the x86 shim, and so is the platform info below:
    // physload reads the map through LegacyPlatformInfo::memory_ranges long
    // after it is called, and it runs, with everything it calls in turn, as
    // nested calls on this stack, which is in this image. Not a static: the
    // translator has a destructor, and a static one would need the C++
    // runtime's exit hooks, which this image does not link.
    bootshim::DevicetreeTranslator translator;
    auto const conversion = translator.convert(tree, own, PAGE_SIZE);
    if (conversion.is_err()) {
        print("[riscv-bootshim] the device tree's memory map cannot be used: ");
        print(conversion_failure(conversion.unwrap_err()));
        print("\n");
        refuse("refusing the device tree", conversion.unwrap_err());
    }
    if (conversion.unwrap().dropped != 0) {
        // A dropped range may be firmware memory inside RAM: passing on the
        // rest would hand it out as free.
        print("[riscv-bootshim] the device tree describes ");
        print_decimal(conversion.unwrap().converted + conversion.unwrap().dropped);
        print(" memory ranges, room for ");
        print_decimal(bootshim::MAX_DEVICETREE_MEMORY_RANGES);
        print("\n");
        refuse("refusing a partial memory map", Status::NoSpace);
    }

    ours::phys::LegacyPlatformInfo const info{
        // A device tree platform describes itself through the tree.
        .acpi_rsdp = ustl::NONE,
        .memory_ranges = translator.ranges().data(),
        .memory_range_count = conversion.unwrap().converted,
        .cmdline = translator.cmdline(),
        // The tree does not name the loader.
        .bootloader_name = {},
        .device_tree = ustl::Option<PhysAddr>(device_tree),
    };
    report(info, translator.initrd());

    print("[riscv-bootshim] platform info ready; physload is not loaded yet, parking\n");
    park();
}
