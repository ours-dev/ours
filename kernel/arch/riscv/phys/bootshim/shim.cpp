/// riscv boot-shim.
///
/// The firmware (OpenSBI) enters the image through `_start` (start.S) with the
/// boot hart's ID and the device tree's physical address. The shim reads the
/// tree, turns it into the memory map physload is handed
/// (bootshim::DevicetreeTranslator), reports that map, the initrd and the
/// command line over the firmware's console, and fills the LegacyPlatformInfo
/// physload's obi_main() takes. It then loads physload, item 0 of the OBI
/// package the firmware loaded as the initrd, into memory the map leaves
/// unclaimed, and calls it, as the x86 shim does with its Multiboot2 module.
///
/// A tree, a package or an image the shim cannot use is refused with the
/// reason, and the hart parked: nothing downstream could do better with a map
/// it could not trust or an image it could not load.

#include <ours/compiler.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/phys/boot_obi.hpp>
#include <ours/phys/elf_image.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/legacy_platform_info.hpp>
#include <ours/prelude.hpp>
#include <ours/status.hpp>

#include <array>
#include <span>
#include <string_view>

#include <arch/riscv/sbi.hpp>
#include <bootshim/devicetree.hpp>
#include <ccrt/runtime.hpp>
#include <fdt/tree.hpp>
#include <obi/container_view.hpp>
#include <obi/package_format.hpp>
#include <ustl/mem/align.hpp>

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

/// Load physload, item 0 of the OBI package the firmware loaded as the initrd,
/// into memory the map leaves unclaimed, and call its entry with the package
/// and `info`.
///
/// physload runs as a nested call on this stack, and reads `info`, the map it
/// points at (in `translator`) and the package for as long as it runs, so both
/// references stay valid: they are the caller's locals, and the caller never
/// returns. physload never returns either; if it did, the shim would have
/// nothing to fall back to.
[[noreturn]]
auto load_physload(bootshim::DevicetreeTranslator const &translator,
                   ours::phys::LegacyPlatformInfo const &info) -> void {
    auto const initrd = translator.initrd();
    if (initrd.is_none()) {
        refuse("no initrd: physload and the rest of the boot travel in it", Status::NotFound);
    }
    auto const package = initrd.value();

    auto const container =
        obi::ContainerView::from(reinterpret_cast<u8 const *>(package.base), package.size);
    if (container.is_err()) {
        refuse("the initrd is not an OBI container", container.unwrap_err());
    }
    auto const boot = ours::phys::BootObi::from(container.unwrap());
    if (boot.is_err()) {
        refuse("the initrd is not an OBI boot package", boot.unwrap_err());
    }
    auto const item = boot.unwrap().executable();
    if (item.is_none()) {
        refuse("the OBI package has no executable item 0 (physload)", Status::NotFound);
    }
    auto const bytes = item.value().payload();

    ours::phys::ElfImage image;
    if (auto const status = image.init({bytes.data(), bytes.size()}); status != Status::Ok) {
        refuse("item 0 is not an ELF image this shim can load", status);
    }

    // Whole pages, page-aligned: physload reserves the memory it runs from in
    // BootMem, which works in pages. find_scratch() keeps it off everything
    // the map claims and off the initrd, which load_at() copies out of.
    auto const size = ustl::round_up(image.size(), PAGE_SIZE);
    auto const target = translator.find_scratch(size, PAGE_SIZE);
    if (target.is_none()) {
        refuse("no unclaimed RAM has room for physload", Status::NoMemory);
    }
    auto const base = PhysAddr(target.value());

    auto const entry = image.load_at(base);
    if (entry.is_err()) {
        refuse("physload could not be loaded", entry.unwrap_err());
    }
    // Identity: phys runs with the MMU off until physload turns it on, and
    // then maps RAM at the same addresses, so the load address is the address
    // physload runs at.
    if (auto const status = image.relocate(base); status != Status::Ok) {
        refuse("physload could not be relocated", status);
    }

    print("[riscv-bootshim] physload at ");
    print_range(base, size);
    print(", entry ");
    print_hex(entry.unwrap());
    print("\n");

    using Entry = void (*)(PhysAddr, usize, ours::phys::LegacyPlatformInfo const *);
    auto const physload = reinterpret_cast<Entry>(entry.unwrap());
    physload(PhysAddr(package.base), static_cast<usize>(package.size), &info);

    print("[riscv-bootshim] physload returned\n");
    park();
}

/// Bit 63 of scause: set for an interrupt, clear for an exception.
constexpr u64 SCAUSE_INTERRUPT = u64{1} << 63;

/// The exception codes S-mode can take, as the privileged specification names
/// them. Codes 10, 11 and 14 are reserved or belong to M-mode.
auto exception_name(u64 const code) -> std::string_view {
    switch (code) {
        case 0:
            return "instruction address misaligned";
        case 1:
            return "instruction access fault";
        case 2:
            return "illegal instruction";
        case 3:
            return "breakpoint";
        case 4:
            return "load address misaligned";
        case 5:
            return "load access fault";
        case 6:
            return "store address misaligned";
        case 7:
            return "store access fault";
        case 8:
            return "environment call from U-mode";
        case 9:
            return "environment call from S-mode";
        case 12:
            return "instruction page fault";
        case 13:
            return "load page fault";
        case 15:
            return "store page fault";
        default:
            return "unknown exception";
    }
}

/// Set on the first entry to bootshim_trap(). The reporter runs on its own
/// fixed stack, so a fault inside it would enter it again from the top, and
/// print over itself forever; the second entry parks instead.
constinit auto TRAPPED = false;

} // namespace

/// Every trap of the phys world ends here (start.S's vector): the shim's own,
/// physload's and physboot's. It says what happened, where, and on what
/// address, and parks; nothing this early can recover.
extern "C" [[noreturn]]
auto bootshim_trap(u64 const cause, u64 const pc, u64 const value, u64 const sp) -> void {
    if (TRAPPED) {
        park();
    }
    TRAPPED = true;

    print("[riscv-phys] trap: ");
    if ((cause & SCAUSE_INTERRUPT) != 0) {
        print("interrupt ");
        print_decimal(cause & ~SCAUSE_INTERRUPT);
    } else {
        print(exception_name(cause));
    }
    print(" (scause ");
    print_hex(cause);
    print(") at sepc ");
    print_hex(pc);
    print(", stval ");
    print_hex(value);
    print(", sp ");
    print_hex(sp);
    print("; halting\n");
    park();
}

/// Called by start.S with a0 and a1 as the firmware left them.
extern "C" [[noreturn]]
auto bootshim_main(usize const hart_id, usize const device_tree) -> void {
    // The phys runtime this image links (the ELF loader, panic) logs through
    // logz4, and the console it logs to registers itself in a static
    // constructor.
    ccrt::init_global_static_objects();
    ours::phys::init_early_console();

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

    load_physload(translator, info);
}
