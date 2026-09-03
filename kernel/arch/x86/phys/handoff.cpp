/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/panic.hpp>
#include <ours/phys/arch_handoff.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>

namespace ours::phys {
namespace {
    /// Raw COM1 write, no logging machinery: boot-step markers have to be
    /// visible even where logz4 is not yet alive (or is the thing under
    /// suspicion).
    auto com1_put(char const c) -> void {
        asm volatile("out %0, %1" : : "a"(c), "d"(u16{0x3F8}));
    }

    /// The switch itself, as bytes:
    ///
    ///   0f 22 d8    mov %rax, %cr3
    ///   ff e2       jmp *%rdx
    constexpr u8 TRAMPOLINE_CODE[] = {0x0F, 0x22, 0xD8, 0xFF, 0xE2};
} // namespace

[[noreturn]]
auto arch_handoff_to_kernel(VirtAddr const entry, PhysAddr const pgd, Handoff *const handoff)
    -> void {
    com1_put('A');

    // One page for the two instructions above, out of bootmem. It needs no
    // mapping: physload identity-mapped every usable range into the very
    // aspace whose root becomes the kernel's CR3, and bootmem only hands out
    // pages inside those ranges -- so the page is executable under both the
    // outgoing and the incoming root (the boot shim identity-maps all of
    // low memory for the outgoing side).
    auto const bootmem = global_bootmem();
    auto const allocated = bootmem->allocate({
        .size = PAGE_SIZE,
        .alignment = PAGE_SIZE,
        .lower = bootmem->min_address(),
        .upper = bootmem->max_address(),
        .node = bootmem::ANY_NODE,
        .direction = bootmem::AllocationDirection::TopDown,
    });
    if (allocated.is_err()) {
        panic("No memory for the kernel handoff trampoline: {}",
              static_cast<u32>(allocated.unwrap_err()));
    }
    auto *const code = reinterpret_cast<u8 *>(allocated.unwrap().base);
    for (usize i = 0; i < sizeof(TRAMPOLINE_CODE); ++i) {
        code[i] = TRAMPOLINE_CODE[i];
    }

    // The handoff pointer doubles as its physical address: physload runs
    // identity-mapped, so the pointer's value is what the kernel receives.
    com1_put('J');
    asm volatile("movq %[pgd], %%rax\n\t"
                 "movq %[entry], %%rdx\n\t"
                 "movq %[handoff], %%rdi\n\t"
                 "call *%[trampoline]\n"
                 :
                 : [pgd] "r"(pgd), [entry] "r"(entry),
                   [handoff] "r"(reinterpret_cast<usize>(handoff)), [trampoline] "r"(code)
                 : "rax", "rdx", "rdi", "memory");

    panic("kernel handoff trampoline returned");
}

} // namespace ours::phys
