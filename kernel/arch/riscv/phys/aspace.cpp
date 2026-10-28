/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/arch/aspace_layout.hpp>
#include <ours/panic.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/init.hpp>

namespace ours::phys {
/// Turns translation on, or moves it to this root, through `satp`: Sv39, the
/// root's physical page number, and address-space ID 0, the only one phys
/// uses.
///
/// The fence before the write orders the page-table stores this aspace just
/// made ahead of the hardware's own reads of the tables: the instruction
/// after `csrw satp` is already fetched through them. The fence after it
/// drops whatever the hart cached under the previous root, Global entries
/// included; when translation was off there is nothing cached, but an
/// install that switches roots needs it.
auto Aspace::arch_install() const -> void {
    auto const satp = ArchLowerPaging::satp(m_lower_pgd, 0);
    asm volatile("sfence.vma\n\t"
                 "csrw satp, %0\n\t"
                 "sfence.vma"
                 :
                 : "r"(satp)
                 : "memory");

    // A hart that does not implement Sv39 ignores the whole write, and would
    // go on running untranslated until the jump to the kernel's high address
    // faulted with nothing to say why.
    u64 installed;
    asm volatile("csrr %0, satp" : "=r"(installed));
    if (installed != satp) {
        panic("satp kept 0x{:X} when 0x{:X} was written: no Sv39 on this hart", installed, satp);
    }
}

/// One root, identity-mapping RAM, and nothing else.
///
/// Everything phys touches is RAM: its own image and stack, the boot package,
/// the device tree, and every page bootmem hands out. The console is a call
/// into the firmware, so unlike x86 there is no device memory to map. RAM is
/// mapped below ARCH_PHYSMAP_SIZE, the same bound the kernel's physmap has,
/// which also keeps the identity map inside Sv39's 256 GiB lower half.
auto arch_setup_aspace(Aspace &aspace) -> void {
    // No bounds: page tables may come from anywhere in bootmem, since nothing
    // on riscv runs translation-off code that has to reach them below some
    // address. Equal bounds are what alloc_page_table() reads as "none". The
    // call is also the only write to those two members: Aspace has no
    // constructor, and physload declares it as a plain local, so without it
    // alloc_page_table() would read whatever the stack held.
    aspace.set_allocation_bounds(0, 0);
    aspace.init();
    aspace.map_usable_ram(ARCH_PHYSMAP_SIZE);
    aspace.install();
}

} // namespace ours::phys
