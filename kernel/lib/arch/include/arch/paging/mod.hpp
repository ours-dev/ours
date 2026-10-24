/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_MOD_HPP
#define ARCH_PAGING_MOD_HPP 1

#include <arch/paging/deposits.hpp>
#include <arch/paging/mapping_context.hpp>
#include <arch/paging/paging_traits.hpp>
#include <arch/paging/visitors.hpp>

#include <atomic>

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
        return map(table, phys_to_table, allocator, NoDeposits{}, va, pa, n, flags, page_size);
    }

    /// Maps [va, va + n pages) to [pa, ...), with a leaf as large as the range
    /// and its alignment allow at each step. With TableDeposits, a leaf above
    /// the final level is mapped only at SECOND_LEVEL and only once
    /// |deposits| has set a table aside for it; when it has none, that part of
    /// the range is mapped with final-level entries instead, in a table from
    /// |allocator|.
    template <typename PhysToVirt, typename Allocator, DepositDiscipline Deposits>
    [[nodiscard]]
    static auto map(PhysAddr table, PhysToVirt &&phys_to_table, Allocator &&allocator,
                    Deposits &&deposits, VirtAddr va, PhysAddr pa, usize n, MmuFlags flags,
                    usize page_size = PAGE_SIZE) -> ustl::Result<void, MapError> {
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
            auto result = map_one_from<Traits::PAGING_LEVEL>(table, phys_to_table, allocator,
                                                             deposits, context);
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
        return unmap(table, phys_to_table, NoDeposits{}, va, n);
    }

    /// Unmaps [va, va + n pages). With TableDeposits, a large leaf that either
    /// end of the range falls inside is split first (split_at()), so every leaf
    /// the walk then meets is covered whole; each such leaf gives one table
    /// back to |deposits|. Without, a large leaf covered in part is InvArgs.
    template <typename PhysToVirt, DepositDiscipline Deposits>
    [[nodiscard]]
    static auto unmap(PhysAddr table, PhysToVirt &&phys_to_table, Deposits &&deposits, VirtAddr va,
                      usize n) -> ustl::Result<usize, MapError> {
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
        split_ends(table, phys_to_table, deposits, va, size);

        auto remaining = n;
        auto unmapped = usize{0};
        auto address = va;
        while (remaining != 0) {
            UnmapVisitor<Deposits> visitor(address, remaining, deposits);
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

    /// Takes permissions away and never gives one: the permission bits of every
    /// leaf in the range become what they were AND |flags|, and no other bit is
    /// touched. An entry that already has less than |flags| keeps what it has.
    ///
    /// This is all protect() does, and it is deliberate that it cannot do
    /// more. An operation that sets the permissions to a value also gives back
    /// what an entry lacked, and an entry lacks a permission for a reason the
    /// caller may not know: a page mapped read-only because other nodes share
    /// it comes out writable, and the next write goes through to all of them.
    /// A permission is added only by mapping the page again, which names the
    /// frame, so that whoever adds it has to have shown the page may have it.
    ///
    /// Returns the number of pages visited, for the TLB invalidation that the
    /// caller owes for each.
    template <typename PhysToVirt>
    [[nodiscard]]
    static auto protect(PhysAddr table, PhysToVirt &&phys_to_table, VirtAddr va, usize n,
                        MmuFlags flags) -> ustl::Result<usize, MapError> {
        return protect(table, phys_to_table, NoDeposits{}, va, n, flags);
    }

    /// protect() with the ends of the range split first, as unmap() splits
    /// them: see there.
    template <typename PhysToVirt, DepositDiscipline Deposits>
    [[nodiscard]]
    static auto protect(PhysAddr table, PhysToVirt &&phys_to_table, Deposits &&deposits,
                        VirtAddr va, usize n, MmuFlags flags) -> ustl::Result<usize, MapError> {
        if (va % PAGE_SIZE == 0 && n != 0 && n <= (~usize{0}) / PAGE_SIZE &&
            n * PAGE_SIZE - 1 <= (~VirtAddr{0}) - va) {
            split_ends(table, phys_to_table, deposits, va, n * PAGE_SIZE);
        }
        return visit_range(table, phys_to_table, va, n, [flags](VirtAddr address, usize remaining) {
            return ProtectVisitor(address, remaining, flags);
        });
    }

    /// What free_tables() did: the table pages it gave back, and the leaves it
    /// stepped over.
    struct TableTeardown {
        usize tables = 0;
        usize leaves = 0;
    };

    /// Frees every table hanging off the top-level entries [first, last] of
    /// the table rooted at |table|, deepest first, clearing each entry that
    /// pointed at one; |deallocate(pa, size)| gives a table back, the mirror
    /// of map()'s allocator. The root table itself is the caller's.
    ///
    /// Leaves are stepped over and counted, never freed: the frames they point
    /// at belong to whoever mapped them -- a VMO -- and a large leaf at an
    /// upper level holds a data frame, not a table, so recursing into it would
    /// free someone's memory as page tables. A count other than zero means the
    /// caller tore the table down with something still mapped.
    ///
    /// Only for a table no CPU has loaded: nothing here flushes a TLB or a
    /// paging-structure cache. Unmapping never frees a table (a table freed
    /// there would have to wait out the flush that retires it), so an address
    /// space accumulates tables for its whole life and they all come back
    /// here, at once, when it is destroyed.
    template <typename PhysToVirt, typename Deallocator>
    static auto free_tables(PhysAddr table, PhysToVirt &&phys_to_table, Deallocator &&deallocate,
                            usize first, usize last) -> TableTeardown {
        return free_tables(table, phys_to_table, deallocate, NoDeposits{}, first, last);
    }

    /// free_tables() that also gives back, with TableDeposits, every table set
    /// aside with a second-level table it frees: those are the tables kept for
    /// leaves that were still mapped, and nothing else will free them.
    template <typename PhysToVirt, typename Deallocator, DepositDiscipline Deposits>
    static auto free_tables(PhysAddr table, PhysToVirt &&phys_to_table, Deallocator &&deallocate,
                            Deposits &&deposits, usize first, usize last) -> TableTeardown {
        TableTeardown result;
        free_below<Traits::PAGING_LEVEL>(table, phys_to_table, deallocate, deposits, first, last,
                                         result);
        return result;
    }

  private:
    // Depth is the number of paging levels, fixed at compile time, so the
    // recursion is bounded by construction rather than by the tree's shape.
    template <Level Level, typename PhysToVirt, typename Deallocator, typename Deposits>
    static auto free_below(PhysAddr pa_table, PhysToVirt &&phys_to_table, Deallocator &&deallocate,
                           Deposits &deposits, usize first, usize last, TableTeardown &result)
        -> void {
        Table<Level> &table = *reinterpret_cast<Table<Level> *>(phys_to_table(pa_table));
        for (auto index = first; index <= last; ++index) {
            auto &entry = table[index];
            if (!entry.is_present()) {
                continue;
            }
            if (entry.is_terminal()) {
                result.leaves += 1;
                continue;
            }
            if constexpr (Level != Traits::FINAL_LEVEL) {
                constexpr auto NEXT = Traits::next_level(Level);
                auto const child = entry.address();
                free_below<NEXT>(child, phys_to_table, deallocate, deposits, 0,
                                 Traits::template NUM_PTES<NEXT> - 1, result);
                if constexpr (NEXT == Traits::SECOND_LEVEL &&
                              TableDeposits<std::remove_cvref_t<Deposits>>) {
                    deposits.discard_all(child);
                }
                entry = {};
                deallocate(child, Traits::template SIZE_OF_TABLE<NEXT>);
                result.tables += 1;
            }
        }
    }

    /// Splits the second-level leaf that |va| or |va + size| falls strictly
    /// inside, if any: the two places a range can cover a leaf only in part,
    /// since every leaf between them is covered whole. Nothing without
    /// TableDeposits, where a part-covered leaf stays InvArgs.
    template <typename PhysToVirt, typename Deposits>
    static auto split_ends(PhysAddr table, PhysToVirt &&phys_to_table, Deposits &deposits,
                           VirtAddr va, usize size) -> void {
        if constexpr (TableDeposits<std::remove_cvref_t<Deposits>>) {
            split_at<Traits::PAGING_LEVEL>(table, phys_to_table, deposits, va);
            // |va + size| wraps to 0 for a range that ends at the top of the
            // address space, and 0 is the start of a leaf, never inside one.
            split_at<Traits::PAGING_LEVEL>(table, phys_to_table, deposits, va + size);
        }
    }

    /// Walks down to the second-level entry for |addr| and, when it is a leaf
    /// that |addr| is not the start of, splits it: its table comes from the
    /// deposits of the second-level table that holds it, is filled with the
    /// final-level entries the leaf stands for -- the same frames, rebuilt
    /// from the same flags -- and replaces the leaf. Nothing is allocated, and
    /// a walker on another CPU sees the leaf or the table, never a table half
    /// filled: the fence orders the fill before the entry that publishes it.
    ///
    /// The flags go through the neutral MmuFlags and back, as they did when the
    /// leaf was made, rather than being copied: an entry's own bits differ by
    /// level (x86's PageSize at the second level is PAT at the final one).
    ///
    /// TLB entries for the leaf are the caller's to invalidate. The new entries
    /// translate exactly as the leaf did, so a CPU still holding the large
    /// translation reads the same memory until the operation that follows the
    /// split changes it and invalidates.
    template <Level Level, typename PhysToVirt, typename Deposits>
    static auto split_at(PhysAddr pa_table, PhysToVirt &&phys_to_table, Deposits &deposits,
                         VirtAddr addr) -> void {
        auto &entry = locate_entry<Level>(pa_table, phys_to_table, addr);
        if (!entry.is_present()) {
            return;
        }
        if constexpr (Level == Traits::SECOND_LEVEL) {
            if (!entry.is_terminal() || addr % Traits::page_size(Level) == 0) {
                return;
            }
            constexpr auto FINAL = Traits::FINAL_LEVEL;
            auto const small = deposits.withdraw(pa_table);
            auto &table = *reinterpret_cast<Table<FINAL> *>(phys_to_table(small));
            auto const flags = MmuFlagsConv<ArchMmuFlags>::to_arch(
                MmuFlagsConv<ArchMmuFlags>::from_arch(entry.arch_mmu_flags()));
            for (usize i = 0; i < Traits::template NUM_PTES<FINAL>; ++i) {
                table[i] = Pte<FINAL>::make(entry.address() + i * PAGE_SIZE, flags, true);
            }
            std::atomic_thread_fence(std::memory_order_release);
            entry = Pte<Level>::make(small, ArchMmuFlags{}, false);
        } else if constexpr (Level != Traits::FINAL_LEVEL) {
            // A leaf above the second level is never mapped with deposits.
            if (entry.is_terminal()) {
                return;
            }
            split_at<Traits::next_level(Level)>(entry.address(), phys_to_table, deposits, addr);
        }
    }

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
            return visitor.template on_terminal<Level>(entry, pa_table);
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
        auto on_terminal(Pte<Level> const &entry, PhysAddr) const -> ustl::Option<QueryResult> {
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

    /// A leaf covered whole is cleared. With TableDeposits, a second-level
    /// leaf gives back the table that was set aside for it; with deposits a
    /// leaf is never covered in part here, since unmap() split the ends first.
    template <typename Deposits>
    class UnmapVisitor final {
        VirtAddr const m_addr;
        usize const m_remaining;
        Deposits &m_deposits;

      public:
        constexpr UnmapVisitor(VirtAddr const addr, usize const remaining, Deposits &deposits)
            : m_addr(addr), m_remaining(remaining), m_deposits(deposits) {}

        template <Level Level>
        [[nodiscard]]
        auto on_absent() const -> ustl::Result<usize, MapError> {
            return ustl::ok(PAGE_SIZE);
        }

        template <Level Level>
        [[nodiscard]]
        auto on_terminal(Pte<Level> &entry, PhysAddr const owner) const
            -> ustl::Result<usize, MapError> {
            auto const page_size = Traits::page_size(Level);
            if (m_addr % page_size != 0 || m_remaining < page_size / PAGE_SIZE) {
                return ustl::err(MapError::InvArgs);
            }
            entry = {};
            if constexpr (Level == Traits::SECOND_LEVEL &&
                          TableDeposits<std::remove_cvref_t<Deposits>>) {
                m_deposits.discard(owner);
            }
            return ustl::ok(page_size);
        }

        template <Level Level>
        [[nodiscard]]
        auto on_final_non_terminal() const -> ustl::Result<usize, MapError> {
            return ustl::err(MapError::InvArgs);
        }
    };

    /// The walk protect() is made of: the leaves of [va, va + n pages),
    /// each visited by a fresh visitor for the address it starts at and the pages
    /// still to do. |make_visitor| makes one.
    template <typename PhysToVirt, typename MakeVisitor>
    [[nodiscard]]
    static auto visit_range(PhysAddr table, PhysToVirt &&phys_to_table, VirtAddr va, usize n,
                            MakeVisitor &&make_visitor) -> ustl::Result<usize, MapError> {
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
        auto visited_pages = usize{0};
        auto address = va;
        while (remaining != 0) {
            auto visitor = make_visitor(address, remaining);
            auto result = walk_levels<Traits::PAGING_LEVEL>(table, phys_to_table, address, visitor);
            if (result.is_err()) {
                return ustl::err(result.error());
            }

            auto const bytes = result.unwrap();
            auto const pages = bytes / PAGE_SIZE;
            address += bytes;
            remaining -= pages;
            visited_pages += pages;
        }

        return ustl::ok(visited_pages);
    }

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
        auto on_terminal(Pte<Level> &entry, PhysAddr) const -> ustl::Result<usize, MapError> {
            auto const page_size = Traits::page_size(Level);
            if (m_addr % page_size != 0 || m_remaining < page_size / PAGE_SIZE) {
                return ustl::err(MapError::InvArgs);
            }
            // Only the permission bits are ANDed; Present, User and the rest of
            // what the entry says are carried through as they are.
            auto const current = MmuFlagsConv<ArchMmuFlags>::from_arch(entry.arch_mmu_flags());
            auto const kept =
                (current & ~MmuFlags::PermMask) | (current & m_flags & MmuFlags::PermMask);
            entry = entry.with_protection(MmuFlagsConv<ArchMmuFlags>::to_arch(kept));
            return ustl::ok(page_size);
        }

        template <Level Level>
        [[nodiscard]]
        auto on_final_non_terminal() const -> ustl::Result<usize, MapError> {
            return ustl::err(MapError::InvArgs);
        }
    };

    /// Whether a leaf may be written at |Level|, for a part of the range that
    /// fits one: any level that can hold one without deposits; with them, the
    /// final level, and the second once a table is set aside for the leaf --
    /// asked last, so that nothing is reserved for a leaf not written.
    template <Level Level, typename Deposits>
    [[nodiscard]]
    static auto may_be_leaf([[maybe_unused]] Deposits &deposits,
                            [[maybe_unused]] PhysAddr const owner, bool const fits) -> bool {
        if (!fits || !Traits::level_can_be_terminal(Level)) {
            return false;
        }
        if constexpr (TableDeposits<std::remove_cvref_t<Deposits>> &&
                      Level != Traits::FINAL_LEVEL) {
            if constexpr (Level != Traits::SECOND_LEVEL) {
                return false;
            } else {
                return deposits.reserve(owner);
            }
        }
        return true;
    }

    template <Level Level, typename PhysToVirt, typename Allocator, typename Deposits>
    [[nodiscard]]
    static auto map_one_from(PhysAddr pa_table, PhysToVirt &&phys_to_table, Allocator &&allocator,
                             Deposits &deposits, MapContext &context)
        -> ustl::Result<void, MapError> {
        auto const address = context.virt_addr();
        auto &entry = locate_entry<Level>(pa_table, phys_to_table, address);

        if (entry.is_present()) {
            if (entry.is_terminal()) {
                return ustl::err(MapError::Existent);
            }
        } else {
            auto const page_size = Traits::page_size(Level);
            auto const physical_address = context.phys_addr();
            auto const fits = page_size <= context.phys_cursor().remaining_size() &&
                              physical_address % page_size == 0 && address % page_size == 0;
            auto const is_terminal = may_be_leaf<Level>(deposits, pa_table, fits);
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
                                                           allocator, deposits, context);
        }
    }
};
} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_MOD_HPP
