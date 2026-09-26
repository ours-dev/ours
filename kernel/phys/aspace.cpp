#include <ours/arch/aspace_layout.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/init.hpp>

#include <cstring>

#include <logz4/log.hpp>

namespace ours::phys {
struct PhysToTable {
    auto operator()(PhysAddr pa_table) -> VirtAddr {
        return VirtAddr(pa_table);
    }
};

auto Aspace::map(VirtAddr va, usize n, PhysAddr pa, MmuFlags flags)
    -> ustl::Result<void, MapError> {
    DEBUG_ASSERT(m_allocation_lower_bound <= m_allocation_upper_bound, "");
    auto allocator = [this](usize size, usize alignment) {
        return alloc_page_table(size, alignment);
    };

    // One root (x86, RISC-V): the root table's low entries translate the lower
    // half and its high ones the upper, so every address -- kernel half
    // included -- is looked up through that one table.
    if constexpr (!DUAL_ASPACE) {
        return LowerPaging::map(m_lower_pgd, PhysToTable(), allocator, va, pa, n, flags);
    } else {
        // Two roots (arm64: TTBR0 for the lower half, TTBR1 for the upper), and
        // the hardware sends an address to exactly one of them.  This used to
        // map every range into *both* tables, indexing each by the same low
        // bits of the address, which produced two wrong mappings at once: the
        // identity map of low RAM also landed in the upper root, giving the
        // kernel half a phantom alias of low physical memory, and the kernel's
        // upper-half mappings also landed in the lower root, i.e. inside the
        // user half.  It was also not unwind-safe -- an upper map that
        // succeeded stayed in the table when the lower one then failed.
        if (n == 0) {
            return ustl::ok();
        }
        // Guard the multiplication before computing the last byte's address.
        if (n > (~usize{0}) / PAGE_SIZE) {
            return ustl::err(MapError::InvArgs);
        }
        auto const last = va + (n * PAGE_SIZE - 1);
        // Wrapped past the top of the address space, or straddling the two
        // halves: no single root can hold the range, and splitting it across
        // both is the bug above, so refuse it rather than guess.
        if (last < va || is_upper(va) != is_upper(last)) {
            return ustl::err(MapError::InvArgs);
        }

        if (is_upper(va)) {
            return UpperPaging::map(m_upper_pgd, PhysToTable(), allocator, va, pa, n, flags);
        }
        return LowerPaging::map(m_lower_pgd, PhysToTable(), allocator, va, pa, n, flags);
    }
}

auto Aspace::unmap(VirtAddr va, usize n) -> Status {
    return Status::NotSupported;
}

auto Aspace::alloc_page_table(usize size, usize alignment) const -> PhysAddr {
    auto const memory = global_bootmem();
    auto const lower = m_allocation_lower_bound < m_allocation_upper_bound
                           ? m_allocation_lower_bound
                           : memory->min_address();
    auto const upper = m_allocation_lower_bound < m_allocation_upper_bound
                           ? m_allocation_upper_bound
                           : memory->max_address();
    auto const allocated = memory->allocate({
        .size = size,
        .alignment = alignment,
        .lower = lower,
        .upper = upper,
        .node = bootmem::ANY_NODE,
        .direction = bootmem::AllocationDirection::TopDown,
    });
    auto const phys = allocated.is_ok() ? allocated.unwrap().base : PhysAddr{};

    if (phys) {
        std::memset(reinterpret_cast<void *>(phys), 0, size);
    }

    log::debug("Allocate page table (p:0x{:X}, s:0x{:X})", phys, size);
    return phys;
}

auto Aspace::map_usable_ram(PhysAddr max_limit) -> void {
    auto const map = [&](PhysAddr base, PhysAddr end) {
        auto const size = end - base;
        log::debug("Map identically (0x{:X}, 0x{:X})", base, size);

        constexpr auto mmuf = MmuFlags::PermMask | MmuFlags::Present;
        auto result = map_identically(base, size / PAGE_SIZE, mmuf);
        if (result.is_err()) {
            panic("Identically map {} pages", to_string(result.unwrap_err()));
        }
    };

    auto const mem = global_bootmem();

    auto start = PhysAddr{};
    auto end = PhysAddr{};
    auto has_range = false;
    auto const status = mem->visit(
        {
            .lower = 0,
            .upper = max_limit,
            .node = bootmem::ANY_NODE,
            .view = bootmem::MemoryView::Usable,
        },
        [&](bootmem::MemoryRegion const region) {
            if (has_range && end != region.range.base) {
                map(start, end);
                has_range = false;
            }
            if (!has_range) {
                start = region.range.base;
                has_range = true;
            }
            end = region.range.end();
            return true;
        });
    if (status != Status::Ok) {
        panic("Failed to enumerate usable RAM: {}", static_cast<u32>(status));
    }
    if (has_range) {
        map(start, end);
    }
}

} // namespace ours::phys
