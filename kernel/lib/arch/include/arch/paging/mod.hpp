/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_MOD_HPP
#define ARCH_PAGING_MOD_HPP 1

#include <arch/paging/controls.hpp>
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

    /// Whether the second-level entry |va| falls under is empty, so that a
    /// second-level leaf could be written there: none of the levels above has
    /// a leaf over it, and the entry itself neither is a leaf nor points at a
    /// table. A table there, even one with nothing mapped in it, is not empty:
    /// a split leaves one behind and nothing frees it.
    template <typename PhysToVirt>
    [[nodiscard]]
    static auto second_level_empty(PhysAddr table, PhysToVirt &&phys_to_table, VirtAddr va)
        -> bool {
        return second_level_empty_from<Traits::PAGING_LEVEL>(table, phys_to_table, va);
    }

    template <typename PhysToVirt, typename Allocator>
    [[nodiscard]]
    static auto map(PhysAddr table, PhysToVirt &&phys_to_table, Allocator &&allocator, VirtAddr va,
                    PhysAddr pa, usize n, MmuFlags flags, MapOptions options,
                    usize *mapped = nullptr, usize page_size = PAGE_SIZE)
        -> ustl::Result<void, MapError> {
        return map(table, phys_to_table, allocator, NoDeposits{}, va, pa, n, flags, options, mapped,
                   page_size);
    }

    /// Maps [va, va + n pages) to [pa, ...), with a leaf as large as the range
    /// and its alignment allow at each step. With TableDeposits, a leaf above
    /// the final level is mapped only at SECOND_LEVEL and only once
    /// |deposits| has set a table aside for it; when it has none, that part of
    /// the range is mapped with final-level entries instead, in a table from
    /// |allocator|.
    ///
    /// |options| is the caller's choice of what happens where a page is
    /// mapped already and when the map fails part way (MapOptions). InvArgs
    /// for a value that is not one bit of each group.
    ///
    /// OverwriteIfExisting writes the new entry over a present leaf of the
    /// size the step maps. A larger leaf the range covers only in part is
    /// split first (split_leaf()), with its deposit: InvArgs without deposits,
    /// as an unmap of part of it is. Where a table of smaller entries already
    /// is, the step goes down into it and writes over those, and the table
    /// stays: a leaf is never put where a table was. Every entry written over
    /// may be in a CPU's TLB; invalidating it, before the old frame goes to
    /// anyone else, is the caller's.
    ///
    /// |mapped|, when given, is how many pages from |va| the call leaves as it
    /// made them: |n| when it succeeds; when it fails, none after
    /// UndoOnFailure, and how far it got after KeepOnFailure, the pages it
    /// skipped included.
    ///
    /// The undo takes away only what the call got through, never a page past
    /// the failure. Invalidating the TLB for it is the caller's: an entry
    /// written and taken away again may have been cached by a CPU meanwhile.
    template <typename PhysToVirt, typename Allocator, DepositDiscipline Deposits>
    [[nodiscard]]
    static auto map(PhysAddr table, PhysToVirt &&phys_to_table, Allocator &&allocator,
                    Deposits &&deposits, VirtAddr va, PhysAddr pa, usize n, MmuFlags flags,
                    MapOptions const options, usize *const mapped = nullptr,
                    usize page_size = PAGE_SIZE) -> ustl::Result<void, MapError> {
        auto const report = [mapped](usize const pages) {
            if (mapped != nullptr) {
                *mapped = pages;
            }
        };
        report(0);
        if (!map_options_valid(options)) {
            return ustl::err(MapError::InvArgs);
        }
        if (n == 0) {
            return ustl::ok();
        }
        if (page_size != PAGE_SIZE || std::to_underlying(va) % page_size != 0 ||
            std::to_underlying(pa) % page_size != 0 || n > (~usize{0}) / page_size) {
            return ustl::err(MapError::InvArgs);
        }
        auto const size = n * page_size;
        if (size - 1 > VirtAddr{~0_usize} - va) {
            return ustl::err(MapError::InvArgs);
        }
        if constexpr (Traits::MAX_PHYS_ADDR_SIZE < sizeof(PhysAddr) * 8) {
            constexpr auto MAX_PHYS_ADDR = PhysAddr{(1_usize << Traits::MAX_PHYS_ADDR_SIZE) - 1};
            if (pa > MAX_PHYS_ADDR || size - 1 > MAX_PHYS_ADDR - pa) {
                return ustl::err(MapError::InvArgs);
            }
        }

        MapContext context(va, &pa, 1, size, flags);
        return map_with(table, phys_to_table, allocator, deposits, options, mapped, va, n, context);
    }

    /// Maps |n| pages at |va| to the frames |frames| lists, one frame a page,
    /// in one walk: the frames need not be contiguous, so every leaf is a
    /// final-level one. |options| and |mapped| as for map(), and so is the
    /// undo of a failure, over the whole call. What a bulk map of scattered
    /// pages asks for, in one call rather than one a page.
    template <typename PhysToVirt, typename Allocator, DepositDiscipline Deposits>
    [[nodiscard]]
    static auto map_frames(PhysAddr table, PhysToVirt &&phys_to_table, Allocator &&allocator,
                           Deposits &&deposits, VirtAddr va, PhysAddr *frames, usize n,
                           MmuFlags flags, MapOptions const options, usize *const mapped = nullptr)
        -> ustl::Result<void, MapError> {
        if (mapped != nullptr) {
            *mapped = 0;
        }
        if (!map_options_valid(options)) {
            return ustl::err(MapError::InvArgs);
        }
        if (n == 0) {
            return ustl::ok();
        }
        if (std::to_underlying(va) % PAGE_SIZE != 0 || n > (~usize{0}) / PAGE_SIZE ||
            n * PAGE_SIZE - 1 > VirtAddr{~0_usize} - va) {
            return ustl::err(MapError::InvArgs);
        }
        for (usize i = 0; i < n; ++i) {
            if (std::to_underlying(frames[i]) % PAGE_SIZE != 0) {
                return ustl::err(MapError::InvArgs);
            }
            if constexpr (Traits::MAX_PHYS_ADDR_SIZE < sizeof(PhysAddr) * 8) {
                constexpr auto MAX_PHYS_ADDR =
                    PhysAddr{(1_usize << Traits::MAX_PHYS_ADDR_SIZE) - 1};
                if (frames[i] > MAX_PHYS_ADDR - (PAGE_SIZE - 1)) {
                    return ustl::err(MapError::InvArgs);
                }
            }
        }
        MapContext context(va, frames, n, PAGE_SIZE, flags);
        return map_with(table, phys_to_table, allocator, deposits, options, mapped, va, n, context);
    }

    template <typename PhysToVirt>
    [[nodiscard]]
    static auto unmap(PhysAddr table, PhysToVirt &&phys_to_table, VirtAddr va, usize n)
        -> ustl::Result<usize, MapError> {
        return unmap(table, phys_to_table, NoDeposits{}, va, n);
    }

    /// Unmaps [va, va + n pages) in one walk (walk_range()). With
    /// TableDeposits, a large leaf an end of the range falls inside is split
    /// as the walk meets it, and each leaf covered whole gives its table back
    /// to |deposits|. Without, a large leaf covered in part is InvArgs.
    /// Returns |n|: the pages the range covers, mapped or not, for the TLB
    /// invalidation the caller owes.
    template <typename PhysToVirt, DepositDiscipline Deposits>
    [[nodiscard]]
    static auto unmap(PhysAddr table, PhysToVirt &&phys_to_table, Deposits &&deposits, VirtAddr va,
                      usize n) -> ustl::Result<usize, MapError> {
        if (n == 0) {
            return ustl::ok(0);
        }
        if (std::to_underlying(va) % PAGE_SIZE != 0 || n > (~usize{0}) / PAGE_SIZE) {
            return ustl::err(MapError::InvArgs);
        }
        auto const size = n * PAGE_SIZE;
        if (size - 1 > VirtAddr{~0_usize} - va) {
            return ustl::err(MapError::InvArgs);
        }
        RangeCursor cursor{va, size};
        ClearLeaf<std::remove_reference_t<Deposits>> action{deposits};
        if (auto result =
                walk_range<Traits::PAGING_LEVEL>(table, phys_to_table, deposits, cursor, action);
            result.is_err()) {
            return ustl::err(result.error());
        }
        return ustl::ok(n);
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

    /// protect() with deposits: a large leaf an end of the range falls inside
    /// is split as the walk meets it, as unmap() splits it. See there.
    template <typename PhysToVirt, DepositDiscipline Deposits>
    [[nodiscard]]
    static auto protect(PhysAddr table, PhysToVirt &&phys_to_table, Deposits &&deposits,
                        VirtAddr va, usize n, MmuFlags flags) -> ustl::Result<usize, MapError> {
        if (n == 0) {
            return ustl::ok(0);
        }
        if (std::to_underlying(va) % PAGE_SIZE != 0 || n > (~usize{0}) / PAGE_SIZE) {
            return ustl::err(MapError::InvArgs);
        }
        auto const size = n * PAGE_SIZE;
        if (size - 1 > VirtAddr{~0_usize} - va) {
            return ustl::err(MapError::InvArgs);
        }
        RangeCursor cursor{va, size};
        NarrowLeaf action{flags};
        if (auto result =
                walk_range<Traits::PAGING_LEVEL>(table, phys_to_table, deposits, cursor, action);
            result.is_err()) {
            return ustl::err(result.error());
        }
        return ustl::ok(n);
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
    template <Level Level, typename PhysToVirt>
    [[nodiscard]]
    static auto second_level_empty_from(PhysAddr pa_table, PhysToVirt &&phys_to_table, VirtAddr va)
        -> bool {
        auto const &entry = locate_entry<Level>(pa_table, phys_to_table, va);
        if (!entry.is_present()) {
            return true;
        }
        if constexpr (Level == Traits::SECOND_LEVEL) {
            return false;
        } else {
            if (entry.is_terminal()) {
                return false;
            }
            return second_level_empty_from<Traits::next_level(Level)>(entry.address(),
                                                                      phys_to_table, va);
        }
    }

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

    /// Splits |entry|, a second-level leaf of the table at |pa_table|: its
    /// table comes from that table's deposits, is filled with the final-level
    /// entries the leaf stands for -- the same frames, rebuilt from the same
    /// flags -- and replaces the leaf. Nothing is allocated, and a walker on
    /// another CPU sees the leaf or the table, never a table half filled: the
    /// fence orders the fill before the entry that publishes it.
    ///
    /// The flags go through the neutral MmuFlags and back, as they did when the
    /// leaf was made, rather than being copied: an entry's own bits differ by
    /// level (x86's PageSize at the second level is PAT at the final one).
    ///
    /// TLB entries for the leaf are the caller's to invalidate. The new entries
    /// translate exactly as the leaf did, so a CPU still holding the large
    /// translation reads the same memory until the operation that follows the
    /// split changes it and invalidates.
    template <typename PhysToVirt, typename Deposits>
    static auto split_leaf(Pte<Traits::SECOND_LEVEL> &entry, PhysAddr const pa_table,
                           PhysToVirt &&phys_to_table, Deposits &deposits) -> void {
        constexpr auto FINAL = Traits::FINAL_LEVEL;
        auto const small = deposits.withdraw(pa_table);
        auto &table = *reinterpret_cast<Table<FINAL> *>(phys_to_table(small));
        auto const flags = MmuFlagsConv<ArchMmuFlags>::to_arch(
            MmuFlagsConv<ArchMmuFlags>::from_arch(entry.arch_mmu_flags()));
        for (usize i = 0; i < Traits::template NUM_PTES<FINAL>; ++i) {
            table[i] = Pte<FINAL>::make(entry.address() + i * PAGE_SIZE, flags, true);
        }
        std::atomic_thread_fence(std::memory_order_release);
        entry = Pte<Traits::SECOND_LEVEL>::make(small, ArchMmuFlags{}, false);
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

    // The walk down to one address, for query(): an absent entry and a
    // non-terminal entry at the final level are both fixed, per-visitor
    // outcomes; a terminal entry gets a per-visitor action; anything
    // else just walks one level deeper. unmap() and protect() cover a
    // range and walk it once with walk_range(); map() keeps its own
    // walk, since an absent entry there can turn into a freshly
    // allocated sub-table and keep walking.
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
                .phys = entry.address() + (std::to_underlying(m_addr) % page_size),
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

    /// Where a walk over a range is: the next address, and the bytes still
    /// to do.
    struct RangeCursor {
        VirtAddr va;
        usize remaining;
    };

    /// Walks the range at |cursor| once, table by table: each table on the
    /// way is entered once, and its entries are stepped through in a loop up
    /// to its end or the range's. For each entry:
    ///
    ///   - absent: the part of the range under it is passed over whole;
    ///   - a leaf the range covers whole: |action| is done to it;
    ///   - a leaf the range covers in part: with TableDeposits, at the second
    ///     level, it is split into the table set aside for it (split_leaf())
    ///     and gone into; otherwise InvArgs, since nothing can be split
    ///     without allocating;
    ///   - a table: gone into.
    ///
    /// A leaf can be covered in part only at the two ends of the range, so a
    /// split happens at most twice a walk, where the walk is anyway.
    /// |cursor| moves on as the range is done, and on an error stays where the
    /// walk stopped: whatever came before it is done.
    template <Level Level, typename PhysToVirt, typename Deposits, typename Action>
    [[nodiscard]]
    static auto walk_range(PhysAddr const pa_table, PhysToVirt &&phys_to_table, Deposits &deposits,
                           RangeCursor &cursor, Action &action) -> ustl::Result<void, MapError> {
        auto &table = *reinterpret_cast<Table<Level> *>(phys_to_table(pa_table));

        // The final level as a tight loop: every present entry is a leaf of
        // one page, covered whole.
        if constexpr (Level == Traits::FINAL_LEVEL) {
            for (auto index = Traits::virt_to_index(Level, cursor.va);
                 index < Traits::max_entries(Level) && cursor.remaining != 0; ++index) {
                auto &entry = table[index];
                if (entry.is_present()) {
                    action.template on_leaf<Level>(entry, pa_table);
                }
                cursor.va += PAGE_SIZE;
                cursor.remaining -= PAGE_SIZE;
            }
            return ustl::ok();
        }

        auto const page_size = Traits::page_size(Level);
        for (auto index = Traits::virt_to_index(Level, cursor.va);
             index < Traits::max_entries(Level) && cursor.remaining != 0; ++index) {
            auto &entry = table[index];
            // The part of the range under this entry.
            auto const offset = std::to_underlying(cursor.va) % page_size;
            auto step = page_size - offset;
            if (step > cursor.remaining) {
                step = cursor.remaining;
            }

            if (!entry.is_present()) {
                cursor.va += step;
                cursor.remaining -= step;
                continue;
            }
            if (entry.is_terminal()) {
                if (offset == 0 && step == page_size) {
                    action.template on_leaf<Level>(entry, pa_table);
                    cursor.va += step;
                    cursor.remaining -= step;
                    continue;
                }
                if constexpr (Level == Traits::SECOND_LEVEL &&
                              TableDeposits<std::remove_cvref_t<Deposits>>) {
                    split_leaf(entry, pa_table, phys_to_table, deposits);
                } else {
                    return ustl::err(MapError::InvArgs);
                }
            }
            if constexpr (Level != Traits::FINAL_LEVEL) {
                auto result = walk_range<Traits::next_level(Level)>(entry.address(), phys_to_table,
                                                                    deposits, cursor, action);
                if (result.is_err()) {
                    return result;
                }
            }
        }
        return ustl::ok();
    }

    /// What unmap() does to a leaf covered whole: clears it. With
    /// TableDeposits, a second-level leaf gives back the table that was set
    /// aside for it.
    template <typename Deposits>
    struct ClearLeaf {
        Deposits &deposits;

        template <Level Level>
        auto on_leaf(Pte<Level> &entry, PhysAddr const pa_table) -> void {
            entry = {};
            if constexpr (Level == Traits::SECOND_LEVEL &&
                          TableDeposits<std::remove_cvref_t<Deposits>>) {
                deposits.discard(pa_table);
            }
        }
    };

    /// What protect() does to a leaf covered whole: only the permission bits
    /// are ANDed with |flags|; Present, User and the rest of what the entry
    /// says are carried through as they are.
    struct NarrowLeaf {
        MmuFlags flags;

        template <Level Level>
        auto on_leaf(Pte<Level> &entry, PhysAddr) const -> void {
            auto const current = MmuFlagsConv<ArchMmuFlags>::from_arch(entry.arch_mmu_flags());
            auto const kept =
                (current & ~MmuFlags::PermMask) | (current & flags & MmuFlags::PermMask);
            entry = entry.with_protection(MmuFlagsConv<ArchMmuFlags>::to_arch(kept));
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

    /// The walk of a map, and what its failure leaves: |context| holds the
    /// range of |n| pages at |va| and its frames.
    template <typename PhysToVirt, typename Allocator, typename Deposits>
    [[nodiscard]]
    static auto map_with(PhysAddr table, PhysToVirt &&phys_to_table, Allocator &&allocator,
                         Deposits &deposits, MapOptions const options, usize *const mapped,
                         VirtAddr const va, usize const n, MapContext &context)
        -> ustl::Result<void, MapError> {
        auto const report = [mapped](usize const pages) {
            if (mapped != nullptr) {
                *mapped = pages;
            }
        };
        auto result = map_range<Traits::PAGING_LEVEL>(table, phys_to_table, allocator, deposits,
                                                      options, context);
        if (result.is_ok()) {
            report(n);
            return result;
        }
        // How far the call got: every entry before the failed one is done,
        // written or skipped; the failed one wrote nothing.
        auto const got = (context.virt_addr() - va) / PAGE_SIZE;
        if (!!(options & MapOptions::KeepOnFailure)) {
            report(got);
        } else if (got != 0) {
            // Whole leaves this call wrote, and with Skip what it stepped
            // over, which may be part of a leaf: a part that cannot be split
            // (no deposits) stays, and is all an undo can fail to take.
            auto _ = unmap(table, phys_to_table, deposits, va, got);
        }
        return result;
    }

    /// Moves |context| on by |bytes|, one frame at a time where its frames
    /// are single pages (map_frames()).
    static auto advance(MapContext &context, usize bytes) -> void {
        while (bytes != 0) {
            auto chunk = context.phys_cursor().remaining_size();
            if (chunk > bytes) {
                chunk = bytes;
            }
            context.consume(chunk);
            bytes -= chunk;
        }
    }

    /// Maps what is left of |context| below the table at |pa_table|, table
    /// by table: each table is entered once and its entries are filled in a
    /// loop up to its end or the range's.
    ///
    ///   - At the final level, a tight loop: one entry a page.
    ///   - Above it, an absent entry gets a leaf when the rest of the range
    ///     fits one (aligned, long enough, and allowed: may_be_leaf()), and
    ///     a table from |allocator| otherwise, which the walk goes down into.
    ///   - A present leaf is what |options| says: skipped, an error, or
    ///     written over. Written over in place when it is the size the step
    ///     maps, keeping its deposit; split with its deposit and gone into
    ///     when it is larger; InvArgs without deposits.
    ///   - A present table is gone into, and its entries are filled; a leaf
    ///     never takes a table's place.
    ///
    /// On x86 a present entry may be changed directly; the TLB is the
    /// caller's (see map()).
    template <Level Level, typename PhysToVirt, typename Allocator, typename Deposits>
    [[nodiscard]]
    static auto map_range(PhysAddr const pa_table, PhysToVirt &&phys_to_table,
                          Allocator &&allocator, Deposits &deposits, MapOptions const options,
                          MapContext &context) -> ustl::Result<void, MapError> {
        auto &table = *reinterpret_cast<Table<Level> *>(phys_to_table(pa_table));

        if constexpr (Level == Traits::FINAL_LEVEL) {
            for (auto index = Traits::virt_to_index(Level, context.virt_addr());
                 index < Traits::max_entries(Level) && context.has_more(); ++index) {
                auto &entry = table[index];
                if (entry.is_present()) {
                    if (!!(options & MapOptions::SkipIfExisting)) {
                        context.consume(PAGE_SIZE);
                        continue;
                    }
                    if (!(options & MapOptions::OverwriteIfExisting)) {
                        return ustl::err(MapError::Existent);
                    }
                }
                entry = Pte<Level>::make(context.phys_addr(), context.mmuflags(), true);
                context.consume(PAGE_SIZE);
            }
            return ustl::ok();
        } else {
            auto const page_size = Traits::page_size(Level);
            for (auto index = Traits::virt_to_index(Level, context.virt_addr());
                 index < Traits::max_entries(Level) && context.has_more(); ++index) {
                auto &entry = table[index];
                auto const address = context.virt_addr();
                auto const physical_address = context.phys_addr();
                // Whether a leaf of this level's size would go here: the rest
                // of the run is that long, and both addresses are aligned.
                auto const fits = page_size <= context.phys_cursor().remaining_size() &&
                                  std::to_underlying(physical_address) % page_size == 0 &&
                                  std::to_underlying(address) % page_size == 0;

                if (entry.is_present() && entry.is_terminal()) {
                    if (!!(options & MapOptions::SkipIfExisting)) {
                        // The rest of the leaf that is there, as far as the
                        // range goes, stays as it is.
                        auto step = page_size - std::to_underlying(address) % page_size;
                        if (step > context.virt_cursor().remaining_size()) {
                            step = context.virt_cursor().remaining_size();
                        }
                        advance(context, step);
                        continue;
                    }
                    if (!(options & MapOptions::OverwriteIfExisting)) {
                        return ustl::err(MapError::Existent);
                    }
                    if (fits) {
                        entry = Pte<Level>::make(physical_address, context.mmuflags(), true);
                        context.consume(page_size);
                        continue;
                    }
                    if constexpr (Level == Traits::SECOND_LEVEL &&
                                  TableDeposits<std::remove_cvref_t<Deposits>>) {
                        split_leaf(entry, pa_table, phys_to_table, deposits);
                    } else {
                        return ustl::err(MapError::InvArgs);
                    }
                } else if (!entry.is_present()) {
                    if (may_be_leaf<Level>(deposits, pa_table, fits)) {
                        entry = Pte<Level>::make(physical_address, context.mmuflags(), true);
                        context.consume(page_size);
                        continue;
                    }
                    auto const sub_table =
                        allocator(Traits::template SIZE_OF_TABLE<Level>, Traits::TABLE_ALIGNMENT);
                    if (sub_table == PhysAddr{}) {
                        return ustl::err(MapError::NoMem);
                    }
                    entry = Pte<Level>::make(sub_table, context.mmuflags(), false);
                }
                auto result = map_range<Traits::next_level(Level)>(
                    entry.address(), phys_to_table, allocator, deposits, options, context);
                if (result.is_err()) {
                    return result;
                }
            }
            return ustl::ok();
        }
    }
};
} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_MOD_HPP
