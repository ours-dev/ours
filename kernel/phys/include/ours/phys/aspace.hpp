/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ASPACE_HPP
#define OURS_PHYS_ASPACE_HPP 1

#include <ours/assert.hpp>
#include <ours/phys/arch_paging.hpp>
#include <ours/types.hpp>

#include <type_traits>

#include <arch/paging.hpp>
#include <ktl/result.hpp>
#include <ustl/option.hpp>

namespace ours::phys {
using MmuFlags = arch::paging::MmuFlags;

class PHYS_EXPORT Aspace {
    using Self = Aspace;

  public:
    using LowerPaging = arch::Paging<ArchLowerPaging>;
    using UpperPaging = arch::Paging<ArchUpperPaging>;
    using MapError = arch::paging::MapError;

    static constexpr auto DUAL_ASPACE = !std::is_same_v<LowerPaging, UpperPaging>;

    /// Which of the two roots translates |va| when there are two (DUAL_ASPACE).
    ///
    /// A two-root scheme describes its lower half as AlwaysZero and its upper
    /// half as AlwaysOne (arch::paging::VirtAddrExt), so the unaddressable
    /// bits -- and therefore the top bit -- are what tell the halves apart;
    /// the static_assert below this class holds the two paging schemes to that.
    /// arm64 itself selects TTBR0 or TTBR1 by bit 55 of the address rather
    /// than bit 63, but the two agree on any address whose top-byte tag is
    /// zero, and phys maps nothing tagged.
    [[nodiscard]]
    static constexpr auto is_upper(VirtAddr const va) -> bool {
        return (std::to_underlying(va) >> 63) != 0;
    }

    auto init() -> void {
        using LowerPagingTraits = LowerPaging::PagingTraits;
        m_lower_pgd =
            alloc_page_table(LowerPagingTraits::SIZE_OF_TABLE<LowerPagingTraits::PAGING_LEVEL>,
                             LowerPagingTraits::TABLE_ALIGNMENT);
        DEBUG_ASSERT(m_lower_pgd != PhysAddr{}, "");

        if constexpr (!DUAL_ASPACE) {
            return;
        }

        typedef UpperPaging::PagingTraits UpperPagingTraits;
        m_upper_pgd =
            alloc_page_table(UpperPagingTraits::SIZE_OF_TABLE<UpperPagingTraits::PAGING_LEVEL>,
                             UpperPagingTraits::TABLE_ALIGNMENT);
        DEBUG_ASSERT(m_upper_pgd != PhysAddr{}, "");
    }

    auto map(VirtAddr va, usize n, PhysAddr pa, MmuFlags flags) -> ustl::Result<void, MapError>;

    FORCE_INLINE
    constexpr auto map_identically(VirtAddr va, usize n, MmuFlags flags)
        -> ustl::Result<void, MapError> {
        return map(va, n, PhysAddr{std::to_underlying(va)}, flags);
    }

    auto unmap(VirtAddr va, usize n) -> Status;

    auto query(VirtAddr va) -> ktl::Result<usize>;

    /// Identically map every usable RAM region below |max_limit| into this
    /// address space, coalescing adjacent bootmem regions into single map
    /// calls.  Must run after memory discovery has registered the RAM.
    auto map_usable_ram(PhysAddr max_limit) -> void;

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

/// is_upper() reads the top bit to pick a root, which is only sound if the two
/// roots really do split the space by that bit: a lower scheme that addressed
/// anything with the top bit set, or an upper one that addressed anything with
/// it clear, would send addresses to the wrong table.  This lives at namespace
/// scope rather than inside map(): map() is not a template, so the branch that
/// `if constexpr` discards is still parsed and its static_asserts still fire,
/// and a single-root arch (x86, VirtAddrExt::Canonical) would fail to build.
static_assert(!Aspace::DUAL_ASPACE ||
                  (Aspace::LowerPaging::PagingTraits::VIRT_ADDR_EXT ==
                       arch::paging::VirtAddrExt::AlwaysZero &&
                   Aspace::UpperPaging::PagingTraits::VIRT_ADDR_EXT ==
                       arch::paging::VirtAddrExt::AlwaysOne),
              "DUAL_ASPACE requires the lower and upper paging schemes to split "
              "the address space by the top bit");

/// Provided by architecture-specific code.
auto arch_setup_aspace(Aspace &aspace) -> void;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_ASPACE_HPP
