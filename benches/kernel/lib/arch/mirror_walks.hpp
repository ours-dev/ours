/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BENCHES_KERNEL_LIB_ARCH_MIRROR_WALKS_HPP
#define BENCHES_KERNEL_LIB_ARCH_MIRROR_WALKS_HPP 1

#include <algorithm>
#include <array>

#include <page_tables/page_tables.hpp>

namespace mirror {
using arch::paging::MapOptions;
using arch::paging::MmuFlags;
using ours::PhysAddr;
using ours::VirtAddr;
using Level = page_tables::PageTableLevel;
/// What an entry is in the kernel being mirrored: a uint64_t, which on x86-64
/// is unsigned long, the type of its cursors' fields too. On the host
/// uint64_t is unsigned long long, a type apart, which would let the compiler
/// keep the cursors in registers where the kernel cannot; usize is unsigned
/// long on both, so the mirror runs here as it would there.
using Entry = usize;

/// Another kernel's x86 page-table walks, rewritten function for function for
/// page-walk-bench, so that their time can be set beside this kernel's own
/// (page_tables::X86PageTableImpl) on the same cases. Which kernel, and what
/// was left out, is in docs/perf/261025/page-walk.md, stage 3's comparison.
///
/// Its shape, as there:
///
///   - one function per operation for the levels above the last, the level
///     an argument, recursing a level a call; one for the last level;
///   - entries read and written through `volatile Entry *`;
///   - a cursor class with the virtual range and, for a map, the frames;
///   - every entry written goes through UpdateEntry() or UnmapEntry(), which
///     queue the address in a pending-invalidation list of 32 items, falling
///     back to a full shootdown;
///   - each table counts its present entries, and unmapping frees a table
///     whose count falls to zero;
///   - a large leaf covered in part is split into a freshly allocated table.
///
/// Left out: the lock, the hardware invalidation and cache flushes (Finish()
/// only empties the list), and the lookup of a table's page descriptor,
/// which is an index into the arena here.
template <typename Arena>
class MirrorTable {
  public:
    MirrorTable(Arena *arena, usize *visits) : m_arena(arena), m_visits(visits) {}

    [[nodiscard]]
    auto map(VirtAddr const va, PhysAddr const pa, usize const n, MmuFlags const flags, MapOptions,
             usize *) -> ours::Status {
        return map_pages_contiguous(va, pa, n, flags);
    }

    [[nodiscard]]
    auto map_frames(VirtAddr const va, PhysAddr const *const frames, usize const n,
                    MmuFlags const flags, MapOptions, usize *) -> ours::Status {
        return map_pages(va, frames, n, flags, ExistingEntryAction::Error);
    }

    [[nodiscard]]
    auto unmap(VirtAddr const va, usize const n) -> ours::Status {
        if (!check_vaddr(va)) {
            return ours::Status::InvArgs;
        }
        if (n == 0) {
            return ours::Status::Ok;
        }
        VirtualAddressCursor cursor(va, n * PAGE_SIZE);
        ConsistencyManager cm;
        auto const [status, lower_unmapped] =
            remove_mapping(root(), Level::PageMapLevel4, CheckForEmptyPt::No, cursor, &cm);
        mappings(m_arena->root()) -= lower_unmapped;
        cm.finish();
        return status;
    }

    [[nodiscard]]
    auto protect(VirtAddr const va, usize const n, MmuFlags const flags) -> ours::Status {
        if (!check_vaddr(va)) {
            return ours::Status::InvArgs;
        }
        if (n == 0) {
            return ours::Status::Ok;
        }
        VirtualAddressCursor cursor(va, n * PAGE_SIZE);
        ConsistencyManager cm;
        auto const status = update_mapping(root(), flags, Level::PageMapLevel4, cursor, &cm);
        cm.finish();
        return status;
    }

  private:
    enum class ExistingEntryAction { Skip, Error, Upgrade };
    enum class CheckForEmptyPt : bool { No, Yes };

    struct Result {
        ours::Status status;
        u32 count;
    };

    /// A virtual range being walked: where it is, and what is left.
    class VirtualAddressCursor {
      public:
        VirtualAddressCursor(VirtAddr const va, usize const size)
            : m_start(va), m_va(va), m_size(size) {}

        auto consume(usize const ps) -> void {
            m_va += ps;
            m_size -= ps;
        }

        /// Moves to the start of the next entry of size |ps|, or to the end.
        auto skip_entry(usize const ps) -> void {
            auto const next = (m_va / ps + 1) * ps;
            consume(std::min(next - m_va, m_size));
        }

        [[nodiscard]]
        auto processed_range() const -> VirtualAddressCursor {
            return VirtualAddressCursor(m_start, m_va - m_start);
        }

        [[nodiscard]]
        auto vaddr() const -> VirtAddr {
            return m_va;
        }

        [[nodiscard]]
        auto size() const -> usize {
            return m_size;
        }

      private:
        VirtAddr m_start;
        VirtAddr m_va;
        usize m_size;
    };

    /// A map's cursor: the virtual range, and frames of |page_size| bytes.
    class MappingCursor {
      public:
        MappingCursor(PhysAddr const *const paddrs, usize const page_size, VirtAddr const va,
                      usize const count)
            : m_paddrs(paddrs), m_page_size(page_size), m_vaddr(va, page_size * count) {}

        auto consume(usize const ps) -> void {
            m_consumed += ps;
            if (m_consumed == m_page_size) {
                m_paddrs++;
                m_consumed = 0;
            }
            m_vaddr.consume(ps);
        }

        [[nodiscard]]
        auto paddr() const -> PhysAddr {
            return *m_paddrs + m_consumed;
        }

        [[nodiscard]]
        auto page_remaining() const -> usize {
            return m_page_size - m_consumed;
        }

        [[nodiscard]]
        auto processed_range() const -> VirtualAddressCursor {
            return m_vaddr.processed_range();
        }

        [[nodiscard]]
        auto vaddr() const -> VirtAddr {
            return m_vaddr.vaddr();
        }

        [[nodiscard]]
        auto size() const -> usize {
            return m_vaddr.size();
        }

      private:
        PhysAddr const *m_paddrs;
        usize m_consumed = 0;
        usize m_page_size;
        VirtualAddressCursor m_vaddr;
    };

    /// Addresses whose translations changed, to be invalidated at the end.
    class ConsistencyManager {
      public:
        auto enqueue(VirtAddr const va, Level const level, bool const is_global,
                     bool const is_terminal) -> void {
            if (is_global) {
                m_contains_global = true;
            }
            if (m_count >= m_items.size() || level == Level::PageMapLevel4) {
                m_full_shootdown = true;
                return;
            }
            m_items[m_count] = (va >> 12 << 12) | (u64(is_terminal) << 4) | (u64(is_global) << 3) |
                               static_cast<u64>(level);
            m_count++;
        }

        auto queue_free(PhysAddr) -> void {
            m_freed++;
        }

        auto finish() -> void {
            m_count = 0;
            m_full_shootdown = false;
            m_contains_global = false;
            asm volatile("" : : "r"(m_items.data()) : "memory");
        }

      private:
        bool m_full_shootdown = false;
        bool m_contains_global = false;
        u32 m_count = 0;
        usize m_freed = 0;
        std::array<u64, 32> m_items;
    };

    static auto page_size(Level const level) -> usize {
        switch (level) {
            case Level::PageTable:
                return usize{1} << 12;
            case Level::PageDirectory:
                return usize{1} << 21;
            case Level::PageDirectoryPointerTable:
                return usize{1} << 30;
            case Level::PageMapLevel4:
                return usize{1} << 39;
            default:
                __builtin_trap();
        }
    }

    static auto page_aligned(Level const level, VirtAddr const va) -> bool {
        return (va & (page_size(level) - 1)) == 0;
    }

    static auto vaddr_to_index(Level const level, VirtAddr const va) -> u32 {
        switch (level) {
            case Level::PageMapLevel4:
                return static_cast<u32>((va >> 39) & 511);
            case Level::PageDirectoryPointerTable:
                return static_cast<u32>((va >> 30) & 511);
            case Level::PageDirectory:
                return static_cast<u32>((va >> 21) & 511);
            case Level::PageTable:
                return static_cast<u32>((va >> 12) & 511);
            default:
                __builtin_trap();
        }
    }

    static auto paddr_from_pte(Level const level, Entry const pte) -> PhysAddr {
        switch (level) {
            case Level::PageDirectoryPointerTable:
                return pte & X86_PDP_FRAME;
            case Level::PageDirectory:
                return pte & X86_PD_FRAME;
            case Level::PageTable:
                return pte & X86_PG_FRAME;
            default:
                __builtin_trap();
        }
    }

    static auto lower_level(Level const level) -> Level {
        return static_cast<Level>(static_cast<usize>(level) - 1);
    }

    static auto is_present(Entry const e) -> bool {
        return (e & X86_MMUF_PRESENT) != 0;
    }

    static auto is_large(Entry const e) -> bool {
        return (e & X86_MMUF_PAGE_SIZE) != 0;
    }

    static auto check_vaddr(VirtAddr const va) -> bool {
        return va % PAGE_SIZE == 0 && va < (VirtAddr{1} << 47);
    }

    static auto check_paddr(PhysAddr const pa) -> bool {
        return pa % PAGE_SIZE == 0 && pa < (PhysAddr{1} << 52);
    }

    static auto intermediate_flags() -> Entry {
        return X86_MMUF_WRITABLE | X86_MMUF_USER;
    }

    /// A user table's leaf bits: not global; the write-back PAT is index 0.
    static auto terminal_flags(Level, MmuFlags const flags) -> Entry {
        Entry bits = 0;
        if (!!(flags & MmuFlags::Writable)) {
            bits |= X86_MMUF_WRITABLE;
        }
        if (!!(flags & MmuFlags::User)) {
            bits |= X86_MMUF_USER;
        }
        if (!(flags & MmuFlags::Executable)) {
            bits |= X86_MMUF_NOEXECUTABLE;
        }
        return bits;
    }

    [[nodiscard]]
    auto root() const -> Entry volatile * {
        *m_visits += 1;
        return reinterpret_cast<Entry volatile *>(m_arena->root());
    }

    [[nodiscard]]
    auto next_table(Entry const pte) const -> Entry volatile * {
        *m_visits += 1;
        return reinterpret_cast<Entry volatile *>(pte & X86_PG_FRAME);
    }

    /// The count of present entries of the table at |pa|: the descriptor
    /// lookup, an index into the arena here.
    [[nodiscard]]
    auto mappings(PhysAddr const pa) -> u32 & {
        return m_mappings[(pa - m_arena->root()) / PAGE_SIZE];
    }

    [[nodiscard]]
    auto mappings(Entry volatile *const table) -> u32 & {
        return mappings(reinterpret_cast<PhysAddr>(table));
    }

    auto update_entry(ConsistencyManager *const cm, Level const level, VirtAddr const va,
                      Entry volatile *const pte, PhysAddr const paddr, Entry const flags,
                      bool const was_terminal) -> void {
        Entry const old = *pte;
        Entry const fresh = paddr | flags | X86_MMUF_PRESENT;
        if ((old & ~Entry(X86_MMUF_ACCESSED | X86_MMUF_DIRTY)) == fresh) {
            return;
        }
        *pte = fresh;
        if (is_present(old)) {
            cm->enqueue(va, level, (old & X86_MMUF_GLOBAL) != 0, was_terminal);
        }
    }

    auto unmap_entry(ConsistencyManager *const cm, Level const level, VirtAddr const va,
                     Entry volatile *const pte, bool const was_terminal) -> void {
        Entry const old = *pte;
        *pte = 0;
        cm->enqueue(va, level, (old & X86_MMUF_GLOBAL) != 0, was_terminal);
    }

    [[nodiscard]]
    auto map_pages_contiguous(VirtAddr const va, PhysAddr const pa, usize const n,
                              MmuFlags const flags) -> ours::Status {
        if (!check_paddr(pa) || !check_vaddr(va)) {
            return ours::Status::InvArgs;
        }
        if (n == 0) {
            return ours::Status::Ok;
        }
        MappingCursor cursor(&pa, n * PAGE_SIZE, va, 1);
        return map_with(cursor, flags, ExistingEntryAction::Error);
    }

    [[nodiscard]]
    auto map_pages(VirtAddr const va, PhysAddr const *const phys, usize const n,
                   MmuFlags const flags, ExistingEntryAction const action) -> ours::Status {
        if (!check_vaddr(va)) {
            return ours::Status::InvArgs;
        }
        for (usize i = 0; i < n; ++i) {
            if (!check_paddr(phys[i])) {
                return ours::Status::InvArgs;
            }
        }
        if (n == 0) {
            return ours::Status::Ok;
        }
        MappingCursor cursor(phys, PAGE_SIZE, va, n);
        return map_with(cursor, flags, action);
    }

    [[nodiscard]]
    auto map_with(MappingCursor &cursor, MmuFlags const flags, ExistingEntryAction const action)
        -> ours::Status {
        ConsistencyManager cm;
        auto const [status, lower_mapped] =
            add_mapping(root(), flags, Level::PageMapLevel4, action, cursor, &cm);
        mappings(m_arena->root()) += lower_mapped;
        if (status != ours::Status::Ok) {
            auto unmap_cursor = cursor.processed_range();
            if (unmap_cursor.size() > 0) {
                auto const [unmap_status, unmapped] = remove_mapping(
                    root(), Level::PageMapLevel4, CheckForEmptyPt::Yes, unmap_cursor, &cm);
                mappings(m_arena->root()) -= unmapped;
            }
        }
        cm.finish();
        return status;
    }

    auto add_mapping(Entry volatile *const table, MmuFlags const flags, Level const level,
                     ExistingEntryAction const action, MappingCursor &cursor,
                     ConsistencyManager *const cm) -> Result {
        if (level == Level::PageTable) {
            return add_mapping_last(table, flags, action, cursor, cm);
        }
        u32 mapped = 0;
        auto const interm_flags = intermediate_flags();
        auto const term_flags = terminal_flags(level, flags);
        auto const ps = page_size(level);
        auto const supports_large = level != Level::PageMapLevel4;
        auto index = vaddr_to_index(level, cursor.vaddr());
        for (; index != 512 && cursor.size() != 0; ++index) {
            Entry volatile *const e = table + index;
            Entry pt_val = *e;

            if (is_present(pt_val) && is_large(pt_val)) {
                if (action == ExistingEntryAction::Error) {
                    return {ours::Status::AlreadyExists, mapped};
                }
                cursor.consume(ps);
                continue;
            }

            auto const valigned = page_aligned(level, cursor.vaddr());
            auto const paligned = page_aligned(level, cursor.paddr());
            if (supports_large && !is_present(pt_val) && valigned && paligned &&
                cursor.page_remaining() >= ps) {
                update_entry(cm, level, cursor.vaddr(), table + index, cursor.paddr(),
                             term_flags | X86_MMUF_PAGE_SIZE, false);
                mapped++;
                cursor.consume(ps);
            } else {
                if (!is_present(pt_val)) {
                    auto const table_paddr = m_arena->allocate(PAGE_SIZE, PAGE_SIZE);
                    if (table_paddr == 0) {
                        cursor.consume(PAGE_SIZE);
                        return {ours::Status::NoMemory, mapped};
                    }
                    update_entry(cm, level, cursor.vaddr(), e, table_paddr, interm_flags, false);
                    mapped++;
                    pt_val = *e;
                }
                auto *const next = next_table(pt_val);
                auto const [status, lower_mapped] =
                    add_mapping(next, flags, lower_level(level), action, cursor, cm);
                if (lower_mapped > 0) {
                    mappings(next) += lower_mapped;
                }
                if (status != ours::Status::Ok) {
                    return {status, mapped};
                }
            }
        }
        return {ours::Status::Ok, mapped};
    }

    auto add_mapping_last(Entry volatile *const table, MmuFlags const flags,
                          ExistingEntryAction const action, MappingCursor &cursor,
                          ConsistencyManager *const cm) -> Result {
        auto const ro = (flags & MmuFlags::PermMask) == MmuFlags::Readable;
        auto const term_flags = terminal_flags(Level::PageTable, flags);
        u32 mapped = 0;
        auto index = vaddr_to_index(Level::PageTable, cursor.vaddr());
        for (; index != 512 && cursor.size() != 0; ++index) {
            Entry volatile *const existing = table + index;
            auto const valid = is_present(*existing);
            if (valid && action == ExistingEntryAction::Error) {
                return {ours::Status::AlreadyExists, mapped};
            }
            auto const paddr_changing = (*existing & X86_PG_FRAME) != cursor.paddr();
            if (valid && action == ExistingEntryAction::Skip) {
                // Left as it is.
            } else if (valid && action == ExistingEntryAction::Upgrade && !paddr_changing) {
                if (!ro) {
                    update_entry(cm, Level::PageTable, cursor.vaddr(), existing, cursor.paddr(),
                                 term_flags, true);
                }
            } else {
                if (!valid) {
                    mapped++;
                }
                if (valid && !ro) {
                    unmap_entry(cm, Level::PageTable, cursor.vaddr(), existing, true);
                }
                update_entry(cm, Level::PageTable, cursor.vaddr(), existing, cursor.paddr(),
                             term_flags, true);
            }
            cursor.consume(PAGE_SIZE);
        }
        return {ours::Status::Ok, mapped};
    }

    auto remove_mapping(Entry volatile *const table, Level const level,
                        CheckForEmptyPt const pt_check, VirtualAddressCursor &cursor,
                        ConsistencyManager *const cm) -> Result {
        if (level == Level::PageTable) {
            return {ours::Status::Ok, remove_mapping_last(table, cursor, cm)};
        }
        u32 unmapped = 0;
        auto const ps = page_size(level);
        auto index = vaddr_to_index(level, cursor.vaddr());
        for (; index != 512 && cursor.size() != 0; ++index) {
            Entry volatile *const e = table + index;
            Entry pt_val = *e;
            if (!is_present(pt_val)) {
                cursor.skip_entry(ps);
                continue;
            }
            if (is_large(pt_val)) {
                if (page_aligned(level, cursor.vaddr()) && cursor.size() >= ps) {
                    unmap_entry(cm, level, cursor.vaddr(), e, true);
                    unmapped++;
                    cursor.consume(ps);
                    continue;
                }
                auto const status = split_large_page(level, cursor.vaddr() & ~(ps - 1), e, cm);
                if (status != ours::Status::Ok) {
                    return {status, unmapped};
                }
                pt_val = *e;
            }
            auto *const next = next_table(pt_val);
            auto const unmap_vaddr = cursor.vaddr();
            auto const [status, lower_unmapped] =
                remove_mapping(next, lower_level(level), pt_check, cursor, cm);
            auto unmap_lower = false;
            if (lower_unmapped > 0 || pt_check == CheckForEmptyPt::Yes) {
                auto &count = mappings(next);
                count -= lower_unmapped;
                unmap_lower = count == 0;
            }
            if (status != ours::Status::Ok) {
                return {status, unmapped};
            }
            if (unmap_lower) {
                unmap_entry(cm, level, unmap_vaddr, e, false);
                unmapped++;
                cm->queue_free(reinterpret_cast<PhysAddr>(next));
            }
        }
        return {ours::Status::Ok, unmapped};
    }

    auto remove_mapping_last(Entry volatile *const table, VirtualAddressCursor &cursor,
                             ConsistencyManager *const cm) -> u32 {
        auto index = vaddr_to_index(Level::PageTable, cursor.vaddr());
        u32 unmapped = 0;
        for (; index != 512 && cursor.size() != 0; ++index) {
            Entry volatile *const e = table + index;
            if (is_present(*e)) {
                unmap_entry(cm, Level::PageTable, cursor.vaddr(), e, true);
                unmapped++;
            }
            cursor.consume(PAGE_SIZE);
        }
        return unmapped;
    }

    auto update_mapping(Entry volatile *const table, MmuFlags const flags, Level const level,
                        VirtualAddressCursor &cursor, ConsistencyManager *const cm)
        -> ours::Status {
        if (level == Level::PageTable) {
            return update_mapping_last(table, flags, cursor, cm);
        }
        auto const term_flags = terminal_flags(level, flags);
        auto const ps = page_size(level);
        auto index = vaddr_to_index(level, cursor.vaddr());
        for (; index != 512 && cursor.size() != 0; ++index) {
            Entry volatile *const e = table + index;
            Entry pt_val = *e;
            if (!is_present(pt_val)) {
                cursor.skip_entry(ps);
                continue;
            }
            if (is_large(pt_val)) {
                if (page_aligned(level, cursor.vaddr()) && cursor.size() >= ps) {
                    update_entry(cm, level, cursor.vaddr(), e, paddr_from_pte(level, pt_val),
                                 term_flags | X86_MMUF_PAGE_SIZE, true);
                    cursor.consume(ps);
                    continue;
                }
                auto const status = split_large_page(level, cursor.vaddr() & ~(ps - 1), e, cm);
                if (status != ours::Status::Ok) {
                    return status;
                }
                pt_val = *e;
            }
            auto const status =
                update_mapping(next_table(pt_val), flags, lower_level(level), cursor, cm);
            if (status != ours::Status::Ok) {
                return status;
            }
        }
        return ours::Status::Ok;
    }

    auto update_mapping_last(Entry volatile *const table, MmuFlags const flags,
                             VirtualAddressCursor &cursor, ConsistencyManager *const cm)
        -> ours::Status {
        auto const term_flags = terminal_flags(Level::PageTable, flags);
        auto index = vaddr_to_index(Level::PageTable, cursor.vaddr());
        for (; index != 512 && cursor.size() != 0; ++index) {
            Entry volatile *const e = table + index;
            Entry const pt_val = *e;
            if (is_present(pt_val)) {
                update_entry(cm, Level::PageTable, cursor.vaddr(), e,
                             paddr_from_pte(Level::PageTable, pt_val), term_flags, true);
            }
            cursor.consume(PAGE_SIZE);
        }
        return ours::Status::Ok;
    }

    /// Splits the large leaf |pte| at |level| into a fresh table of the
    /// level below, with the same frames and permissions.
    auto split_large_page(Level const level, VirtAddr const va, Entry volatile *const pte,
                          ConsistencyManager *const cm) -> ours::Status {
        auto const table_paddr = m_arena->allocate(PAGE_SIZE, PAGE_SIZE);
        if (table_paddr == 0) {
            return ours::Status::NoMemory;
        }
        auto *const table = reinterpret_cast<Entry volatile *>(table_paddr);
        auto const lower = lower_level(level);
        auto const ps = page_size(lower);
        auto flags = (*pte & ~Entry(X86_PG_FRAME)) & ~Entry(X86_MMUF_PRESENT);
        if (lower == Level::PageTable) {
            flags &= ~Entry(X86_MMUF_PAGE_SIZE);
        }
        auto const base = paddr_from_pte(level, *pte);
        for (u32 i = 0; i < 512; ++i) {
            table[i] = (base + i * ps) | flags | X86_MMUF_PRESENT;
        }
        mappings(table_paddr) = 512;
        update_entry(cm, level, va, pte, table_paddr, intermediate_flags(), true);
        return ours::Status::Ok;
    }

    Arena *m_arena;
    usize *m_visits;
    std::array<u32, Arena::CAPACITY> m_mappings{};
};

} // namespace mirror

#endif // #ifndef BENCHES_KERNEL_LIB_ARCH_MIRROR_WALKS_HPP
