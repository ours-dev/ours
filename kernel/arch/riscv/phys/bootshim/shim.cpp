/// riscv boot-shim.
///
/// The firmware (OpenSBI) enters the image through `_start` (start.S) with the
/// boot hart's ID and the device tree's physical address. This stage only
/// proves it was reached: it reports both over the firmware's console and
/// parks the hart. Everything past that -- reading the device tree, finding
/// physload and jumping to it -- is added behind this entry point.

#include <ours/prelude.hpp>

#include <arch/riscv/sbi.hpp>

namespace {

/// The console is the firmware's, one byte per call, so printing is a loop.
/// There is no formatting runtime this early.
auto print(char const *const text) -> void {
    for (auto it = text; *it != '\0'; ++it) {
        arch::sbi::console_putchar(*it);
    }
}

auto print_hex(usize const value) -> void {
    constexpr char DIGITS[] = "0123456789abcdef";
    constexpr usize NIBBLE_BITS = 4;
    constexpr usize NIBBLE_COUNT = sizeof(usize) * 8 / NIBBLE_BITS;

    print("0x");
    for (auto i = NIBBLE_COUNT; i > 0; --i) {
        auto const shift = (i - 1) * NIBBLE_BITS;
        arch::sbi::console_putchar(DIGITS[(value >> shift) & 0xf]);
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

    // Nothing to hand off to yet. wfi returns on any pending interrupt, which
    // are all masked, so this sleeps; the loop is for a spurious wake.
    while (true) {
        asm volatile("wfi" ::: "memory");
    }
}
