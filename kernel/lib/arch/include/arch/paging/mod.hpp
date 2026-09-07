/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_MOD_HPP
#define ARCH_PAGING_MOD_HPP 1

#include <arch/paging/mapping_context.hpp>
#include <arch/paging/paging_traits.hpp>
#include <arch/paging/visitors.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace arch::paging {
template <typename ArchPaging>
class Paging final {
    using Self = Paging<ArchPaging>;

  public:
    using Traits = PagingTraits<ArchPaging>;
    using PagingTraits = Traits;
    using Level = typename Traits::LevelType;

    // ArchPaging names its own PTE flag type; everything below (the walk
    // contexts, MmuFlagsConv<...>) binds to it from here, so no neutral
    // header has to know which architecture is in play.
    using ArchMmuFlags = typename Traits::ArchMmuFlags;
    using TravelContext = BasicTravelContext<ArchMmuFlags>;
    using MapContext = BasicMapContext<ArchMmuFlags>;

    template <Level Level>
    using Pte = typename Traits::template Pte<Level>;

    template <Level Level>
    using Table = typename Traits::template Table<Level>;

    template <typename PhysToVirt>
    [[nodiscard]]
    static auto query(PhysAddr table, PhysToVirt &&phys_to_table, VirtAddr va)
        -> ustl::Option<QueryResult> {
        QueryVisitor visitor(va);
        return walk_levels<Traits::PAGING_LEVEL>(table, phys_to_table, va, visitor);
    }

    template <typename PhysToVirt, typename Allocator>
    [[nodiscard]]
    static auto map(PhysAddr table, PhysToVirt &&phys_to_table, Allocator &&allocator, VirtAddr va,
                    PhysAddr pa, usize n, MmuFlags flags, usize page_size = PAGE_SIZE)
        -> ustl::Result<void, MapError> {
        if (n == 0) {
            return ustl::ok();
        }
        if (page_size != PAGE_SIZE || va % page_size != 0 || pa % page_size != 0 ||
            n > (~usize{0}) / page_size) {
            return ustl::err(MapError::InvArgs);
        }
        auto const size = n * page_size;
        if (size - 1 > (~VirtAddr{0}) - va) {
            return ustl::err(MapError::InvArgs);
        }
        if constexpr (Traits::MAX_PHYS_ADDR_SIZE < sizeof(PhysAddr) * 8) {
            constexpr auto max_phys_addr = (PhysAddr{1} << Traits::MAX_PHYS_ADDR_SIZE) - 1;
            if (pa > max_phys_addr || size - 1 > max_phys_addr - pa) {
                return ustl::err(MapError::InvArgs);
            }
        }

        MapContext context(va, &pa, 1, size, flags);
        while (context.has_more()) {
            auto result =
                map_one_from<Traits::PAGING_LEVEL>(table, phys_to_table, allocator, context);
            if (result.is_err()) {
                return result;
            }
        }

        return ustl::ok();
    }

    template <typename PhysToVirt>
    [[nodiscard]]
    static auto unmap(PhysAddr table, PhysToVirt &&phys_to_table, VirtAddr va, usize n)
        -> ustl::Result<usize, MapError> {
        if (n == 0) {
            return ustl::ok(0);
        }
        if (va % PAGE_SIZE != 0 || n > (~usize{0}) / PAGE_SIZE) {
            return ustl::err(MapError::InvArgs);
        }
        auto const size = n * PAGE_SIZE;
        if (size - 1 > (~VirtAddr{0}) - va) {
            return ustl::err(MapError::InvArgs);
        }

        auto remaining = n;
        auto unmapped = usize{0};
        auto address = va;
        while (remaining != 0) {
            UnmapVisitor visitor(address, remaining);
            auto result = walk_levels<Traits::PAGING_LEVEL>(table, phys_to_table, address, visitor);
            if (result.is_err()) {
                return ustl::err(result.error());
            }

            auto const bytes = result.unwrap();
            auto const pages = bytes / PAGE_SIZE;
            address += bytes;
            remaining -= pages;
            unmapped += pages;
        }

        return ustl::ok(unmapped);
    }

    template <typename PhysToVirt>
    [[nodiscard]]
    static auto protect(PhysAddr table, PhysToVirt &&phys_to_table, VirtAddr va, usize n,
                        MmuFlags flags) -> ustl::Result<usize, MapError> {
        if (n == 0) {
            return ustl::ok(0);
        }
        if (va % PAGE_SIZE != 0 || n > (~usize{0}) / PAGE_SIZE) {
            return ustl::err(MapError::InvArgs);
        }
        auto const size = n * PAGE_SIZE;
        if (size - 1 > (~VirtAddr{0}) - va) {
            return ustl::err(MapError::InvArgs);
        }

        auto remaining = n;
        auto protected_pages = usize{0};
        auto address = va;
        while (remaining != 0) {
            ProtectVisitor visitor(address, remaining, flags);
            auto result = walk_levels<Traits::PAGING_LEVEL>(table, phys_to_table, address, visitor);
            if (result.is_err()) {
                return ustl::err(result.error());
            }

            auto const bytes = result.unwrap();
            auto const pages = bytes / PAGE_SIZE;
            address += bytes;
            remaining -= pages;
            protected_pages += pages;
        }

        return ustl::ok(protected_pages);
    }

  private:
    // Locates the PTE for `addr` at `Level` within the table rooted at
    // `pa_table`. Identical for every operation below — it's the one
    // piece of the walk that never varies.
    template <Level Level, typename PhysToVirt>
    [[nodiscard]]
    static auto locate_entry(PhysAddr pa_table, PhysToVirt &&phys_to_table, VirtAddr addr)
        -> Pte<Level> & {
        auto const index = Traits::virt_to_index(Level, addr);
        Table<Level> &table = *reinterpret_cast<Table<Level> *>(phys_to_table(pa_table));
        return table[index];
    }

    // Shared shape of query/unmap/protect: an absent entry and a
    // non-terminal entry at the final level are both fixed, per-visitor
    // outcomes; a terminal entry gets a per-visitor action; anything
    // else just walks one level deeper. `map` doesn't fit this shape —
    // an absent entry there can turn into a freshly allocated
    // sub-table and keep walking — so it keeps its own function below,
    // built on the same `locate_entry`.
    template <Level Level, typename PhysToVirt, typename Visitor>
    [[nodiscard]]
    static auto walk_levels(PhysAddr pa_table, PhysToVirt &&phys_to_table, VirtAddr addr,
                            Visitor &visitor) -> decltype(visitor.template on_absent<Level>()) {
        auto &entry = locate_entry<Level>(pa_table, phys_to_table, addr);
        if (!entry.is_present()) {
            return visitor.template on_absent<Level>();
        }
        if (entry.is_terminal()) {
            return visitor.template on_terminal<Level>(entry);
        }
        if constexpr (Level == Traits::FINAL_LEVEL) {
            return visitor.template on_final_non_terminal<Level>();
        } else {
            return walk_levels<Traits::next_level(Level)>(entry.address(), phys_to_table, addr,
                                                          visitor);
        }
    }

    class QueryVisitor final {
        VirtAddr const m_addr;

      public:
        constexpr explicit QueryVisitor(VirtAddr const addr) : m_addr(addr) {}

        template <Level Level>
        [[nodiscard]]
        auto on_absent() const -> ustl::Option<QueryResult> {
            return ustl::NONE;
        }

        template <Level Level>
        [[nodiscard]]
        auto on_terminal(Pte<Level> const &entry) const -> ustl::Option<QueryResult> {
            auto const page_size = Traits::page_size(Level);
            return ustl::Option<QueryResult>(QueryResult{
                .phys = entry.address() + (m_addr % page_size),
                .size = page_size,
                .flags = MmuFlagsConv<ArchMmuFlags>::from_arch(entry.arch_mmu_flags()),
            });
        }

        template <Level Level>
        [[nodiscard]]
        auto on_final_non_terminal() const -> ustl::Option<QueryResult> {
            return ustl::NONE;
        }
    };

    class UnmapVisitor final {
        VirtAddr const m_addr;
        usize const m_remaining;

      public:
        constexpr UnmapVisitor(VirtAddr const addr, usize const remaining)
            : m_addr(addr), m_remaining(remaining) {}

        template <Level Level>
        [[nodiscard]]
        auto on_absent() const -> ustl::Result<usize, MapError> {
            return ustl::ok(PAGE_SIZE);
        }

        template <Level Level>
        [[nodiscard]]
        auto on_terminal(Pte<Level> &entry) const -> ustl::Result<usize, MapError> {
            auto const page_size = Traits::page_size(Level);
            if (m_addr % page_size != 0 || m_remaining < page_size / PAGE_SIZE) {
                return ustl::err(MapError::InvArgs);
            }
            entry = {};
            return ustl::ok(page_size);
        }

        template <Level Level>
        [[nodiscard]]
        auto on_final_non_terminal() const -> ustl::Result<usize, MapError> {
            return ustl::err(MapError::InvArgs);
        }
    };

    class ProtectVisitor final {
        VirtAddr const m_addr;
        usize const m_remaining;
        MmuFlags const m_flags;

      public:
        constexpr ProtectVisitor(VirtAddr const addr, usize const remaining, MmuFlags const flags)
            : m_addr(addr), m_remaining(remaining), m_flags(flags) {}

        template <Level Level>
        [[nodiscard]]
        auto on_absent() const -> ustl::Result<usize, MapError> {
            return ustl::ok(PAGE_SIZE);
        }

        template <Level Level>
        [[nodiscard]]
        auto on_terminal(Pte<Level> &entry) const -> ustl::Result<usize, MapError> {
            auto const page_size = Traits::page_size(Level);
            if (m_addr % page_size != 0 || m_remaining < page_size / PAGE_SIZE) {
                return ustl::err(MapError::InvArgs);
            }
            entry = entry.with_protection(MmuFlagsConv<ArchMmuFlags>::to_arch(m_flags));
            return ustl::ok(page_size);
        }

        template <Level Level>
        [[nodiscard]]
        auto on_final_non_terminal() const -> ustl::Result<usize, MapError> {
            return ustl::err(MapError::InvArgs);
        }
    };

    template <Level Level, typename PhysToVirt, typename Allocator>
    [[nodiscard]]
    static auto map_one_from(PhysAddr pa_table, PhysToVirt &&phys_to_table, Allocator &&allocator,
                             MapContext &context) -> ustl::Result<void, MapError> {
        auto const address = context.virt_addr();
        auto &entry = locate_entry<Level>(pa_table, phys_to_table, address);

        if (entry.is_present()) {
            if (entry.is_terminal()) {
                return ustl::err(MapError::Existent);
            }
        } else {
            auto const page_size = Traits::page_size(Level);
            auto const physical_address = context.phys_addr();
            auto const is_terminal = Traits::level_can_be_terminal(Level) &&
                                     page_size <= context.phys_cursor().remaining_size() &&
                                     physical_address % page_size == 0 && address % page_size == 0;
            PhysAddr entry_address;
            if (is_terminal) {
                entry_address = physical_address;
            } else {
                entry_address =
                    allocator(Traits::template SIZE_OF_TABLE<Level>, Traits::TABLE_ALIGNMENT);
            }
            if (!is_terminal && !entry_address) {
                return ustl::err(MapError::NoMem);
            }
            entry = Pte<Level>::make(entry_address, context.mmuflags(), is_terminal);
            if (is_terminal) {
                context.consume(page_size);
                return ustl::ok();
            }
        }

        if constexpr (Level == Traits::FINAL_LEVEL) {
            return ustl::err(MapError::Existent);
        } else {
            return map_one_from<Traits::next_level(Level)>(entry.address(), phys_to_table,
                                                           allocator, context);
        }
    }
};
} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_MOD_HPP
