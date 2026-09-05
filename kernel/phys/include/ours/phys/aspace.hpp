/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ASPACE_HPP
#define OURS_PHYS_ASPACE_HPP 1

#include <arch/paging.hpp>
#include <ktl/result.hpp>
#include <ours/assert.hpp>
#include <ours/phys/arch_paging.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>

#include <limits>
#include <type_traits>

namespace ours::phys {
using MmuFlags = arch::paging::MmuFlags;

class PHYS_EXPORT Aspace {
    using Self = Aspace;

  public:
    using LowerPaging = arch::Paging<ArchLowerPaging>;
    using UpperPaging = arch::Paging<ArchUpperPaging>;
    using MapError = arch::paging::MapError;

    static constexpr auto DUAL_ASPACE = !std::is_same_v<LowerPaging, UpperPaging>;

    auto init() -> void {
        using LowerPagingTraits = LowerPaging::PagingTraits;
        m_lower_pgd =
            alloc_page_table(LowerPagingTraits::SIZE_OF_TABLE<LowerPagingTraits::PAGING_LEVEL>,
                             LowerPagingTraits::TABLE_ALIGNMENT);
        DEBUG_ASSERT(m_lower_pgd, "");

        if constexpr (!DUAL_ASPACE) {
            return;
        }

        typedef UpperPaging::PagingTraits UpperPagingTraits;
        m_upper_pgd =
            alloc_page_table(UpperPagingTraits::SIZE_OF_TABLE<UpperPagingTraits::PAGING_LEVEL>,
                             UpperPagingTraits::TABLE_ALIGNMENT);
        DEBUG_ASSERT(m_lower_pgd, "");
    }

    auto map(VirtAddr va, usize n, PhysAddr pa, MmuFlags flags) -> ustl::Result<void, MapError>;

    FORCE_INLINE
    constexpr auto map_identically(VirtAddr va, usize n, MmuFlags flags)
        -> ustl::Result<void, MapError> {
        return map(va, n, va, flags);
    }

    auto unmap(VirtAddr va, usize n) -> Status;

    auto query(VirtAddr va) -> ktl::Result<usize>;

    FORCE_INLINE
    constexpr auto pgd() -> PhysAddr {
        return m_lower_pgd;
    }

    FORCE_INLINE
    constexpr auto lower_pgd() -> PhysAddr {
        return m_lower_pgd;
    }

    FORCE_INLINE
    constexpr auto upper_pgd() -> PhysAddr {
        return m_upper_pgd;
    }

    FORCE_INLINE
    constexpr auto install() const -> void {
        arch_install();
    }

    FORCE_INLINE
    constexpr auto install(PhysAddr new_pgd) -> void {
        m_lower_pgd = new_pgd;
        arch_install();
    }

    FORCE_INLINE
    constexpr auto install(PhysAddr lower_pgd, PhysAddr upper_pgd) -> void {
        m_lower_pgd = lower_pgd;
        m_upper_pgd = upper_pgd;
        arch_install();
    }

    FORCE_INLINE
    constexpr auto set_allocation_bounds(PhysAddr lower_bound, PhysAddr upper_bound) -> void {
        m_allocation_lower_bound = lower_bound;
        m_allocation_upper_bound = upper_bound;
    }

  private:
    /// Arch-implementation
    auto arch_install() const -> void;
    auto alloc_page_table(usize size, usize alignment) const -> PhysAddr;

    PhysAddr m_lower_pgd;
    PhysAddr m_upper_pgd;
    PhysAddr m_allocation_lower_bound;
    PhysAddr m_allocation_upper_bound;
};

/// Provided by architecture-specific code.
auto arch_setup_aspace(Aspace &aspace) -> void;

/// Identically map the ram memory under |max_limit| to virtual address space.
auto create_identity_map_for_ram(Aspace &aspace, PhysAddr = std::numeric_limits<PhysAddr>::max())
    -> void;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_ASPACE_HPP
