#include <arch/x86/system.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/init.hpp>

#include <algorithm>
namespace ours::phys {
auto Aspace::arch_install() const -> void {
    // Create a shadow of CR4
    auto cr4 = arch::Cr4::read();

    // Disable global paging.
    cr4.set<cr4.Pge>(0);
    arch::Cr4::write(cr4);

    // Reload CR3 and flush all TLB.
    arch::Cr3::from_value(m_lower_pgd).write();

    // Enable global paging.
    cr4.set<cr4.Pge>(1);
    arch::Cr4::write(cr4);
}

typedef Aspace::LowerPaging::PagingTraits Paging;

auto arch_setup_aspace(Aspace &aspace) -> void {
    // Restricting the root page table to the [0, 4GB) range facilitates the transition
    // of APs from protected mode to long mode during boot.
    aspace.set_allocation_bounds(0, GB(4));
    aspace.init();

    // Cancel the allocation bounds of page table and create identity map of RAM under ARCH_PHYSMAP_SIZE.
    aspace.set_allocation_bounds(0, 0);
    create_identity_map_for_ram(aspace, ARCH_PHYSMAP_SIZE);

    // The legacy VGA/BIOS hole sits outside usable RAM, so the RAM walk
    // above skips it — but the phys console still writes the text buffer at
    // 0xB8000 after install(), and a write into an unmapped page triple
    // faults (the early IDT is empty). Map the whole hole 1:1: [0xA0000,
    // 0xC0000) covers the text buffer and the planar video memory.
    if (auto const mapped = aspace.map_identically(0xA'0000, 0x2'0000 / PAGE_SIZE,
                                                   MmuFlags::PermMask | MmuFlags::Present);
        mapped.is_err()) {
        panic("Failed to map VGA memory: {}", to_string(mapped.unwrap_err()));
    }

    aspace.install();
}

} // namespace ours::phys
