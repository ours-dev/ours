#include <ours/arch/aspace_layout.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/print.hpp>

#include <cstring>

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

    if constexpr (kDualAspace) {
        auto upper_result =
            UpperPaging::map(m_upper_pgd, PhysToTable(), allocator, va, pa, n, flags);
        if (!upper_result) {
            return ustl::err(upper_result.unwrap_err());
        }
    }

    auto result = LowerPaging::map(m_lower_pgd, PhysToTable(), allocator, va, pa, n, flags);
    if (!result) {
        return ustl::err(result.unwrap_err());
    }
    DEBUG_ASSERT(result, "Failed to map");

    return ustl::ok();
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
        .node = bootmem::kAnyNode,
        .direction = bootmem::AllocationDirection::TopDown,
    });
    auto const phys = allocated.is_ok() ? allocated.unwrap().base : PhysAddr{};

    if (phys) {
        std::memset(reinterpret_cast<void *>(phys), 0, size);
    }

    dprintln("Allocate page table (p:0x{:X}, s:0x{:X})", phys, size);
    return phys;
}

auto create_identity_map_for_ram(Aspace &aspace, PhysAddr max_limit) -> void {
    auto const map = [&](PhysAddr base, PhysAddr end) {
        auto const size = end - base;
        dprintln("Map identically (0x{:X}, 0x{:X})", base, size);

        constexpr auto const mmuf = MmuFlags::PermMask | MmuFlags::Present;
        auto result = aspace.map_identically(base, size / PAGE_SIZE, mmuf);
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
            .node = bootmem::kAnyNode,
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
