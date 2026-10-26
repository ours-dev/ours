/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <arch/paging.hpp>
#include <arch/paging/x86_pagings.hpp>

namespace {
using namespace arch::paging;

using X86Paging = PagingDispatcher<X86PagingLevel::PageMapLevel4>::Type;
using PageMap = arch::Paging<X86Paging>;

struct PageTablePool {
    alignas(PAGE_SIZE) unsigned char tables[4][PAGE_SIZE] = {};
    usize next = 0;

    auto allocate(usize size, usize alignment) -> ours::PhysAddr {
        if (size != PAGE_SIZE || alignment != PAGE_SIZE || next == 4) {
            return 0;
        }
        return reinterpret_cast<ours::PhysAddr>(tables[next++]);
    }
};

/// What every map of a range nothing has mapped yet asks for.
constexpr auto FRESH = MapOptions::ErrorIfExisting | MapOptions::UndoOnFailure;

struct IdentityPhysToVirt {
    auto operator()(ours::PhysAddr address) const -> ours::VirtAddr {
        return address;
    }
};

auto verify_mapping(ours::VirtAddr virtual_address, ours::PhysAddr physical_address, usize pages,
                    usize expected_page_size) -> bool {
    alignas(PAGE_SIZE) unsigned char root[PAGE_SIZE] = {};
    PageTablePool pool;
    auto allocator = [&pool](usize size, usize alignment) {
        return pool.allocate(size, alignment);
    };

    auto const flags = MmuFlags::Present | MmuFlags::Writable | MmuFlags::Executable;
    auto result = PageMap::map(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                               allocator, virtual_address, physical_address, pages, flags, FRESH);
    if (result.is_err()) {
        return false;
    }

    auto query = PageMap::query(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                                virtual_address);
    // x86 page tables have no read-disable bit: every present mapping reads
    // back as readable, regardless of whether callers named that implied bit.
    auto const canonical_flags = flags | MmuFlags::Readable;
    return query && query.value().phys == physical_address &&
           query.value().size == expected_page_size && query.value().flags == canonical_flags;
}

auto verify_empty_and_invalid_ranges() -> bool {
    alignas(PAGE_SIZE) unsigned char root[PAGE_SIZE] = {};
    PageTablePool pool;
    auto allocator = [&pool](usize size, usize alignment) {
        return pool.allocate(size, alignment);
    };
    auto const flags = MmuFlags::Present | MmuFlags::Writable;

    auto empty = PageMap::map(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                              allocator, 0, 0, 0, flags, FRESH);
    if (empty.is_err() || pool.next != 0) {
        return false;
    }
    auto unaligned = PageMap::map(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                                  allocator, 1, 0, 1, flags, FRESH);
    return unaligned.is_err() && unaligned.error() == MapError::InvArgs;
}

auto verify_unmapping_removes_an_existing_mapping() -> bool {
    alignas(PAGE_SIZE) unsigned char root[PAGE_SIZE] = {};
    PageTablePool pool;
    auto allocator = [&pool](usize size, usize alignment) {
        return pool.allocate(size, alignment);
    };
    auto const virtual_address = 4_mb;
    auto const physical_address = 6_mb;
    auto const flags = MmuFlags::Present | MmuFlags::Writable;

    auto const mapped = PageMap::map(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                                     allocator, virtual_address, physical_address, 1, flags, FRESH);
    if (mapped.is_err()) {
        return false;
    }

    auto const unmapped = PageMap::unmap(reinterpret_cast<ours::PhysAddr>(root),
                                         IdentityPhysToVirt{}, virtual_address, 1);
    if (unmapped.is_err() || unmapped.unwrap() != 1) {
        return false;
    }

    return !PageMap::query(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                           virtual_address);
}

auto verify_protection_updates_an_existing_mapping() -> bool {
    alignas(PAGE_SIZE) unsigned char root[PAGE_SIZE] = {};
    PageTablePool pool;
    auto allocator = [&pool](usize size, usize alignment) {
        return pool.allocate(size, alignment);
    };
    auto const virtual_address = 8_mb;
    auto const physical_address = 10_mb;
    auto const initial_flags =
        MmuFlags::Present | MmuFlags::Writable | MmuFlags::User | MmuFlags::Executable;

    auto const mapped =
        PageMap::map(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{}, allocator,
                     virtual_address, physical_address, 1, initial_flags, FRESH);
    if (mapped.is_err()) {
        return false;
    }

    auto const final_flags = MmuFlags::Present | MmuFlags::Readable | MmuFlags::User;
    auto const protected_pages =
        PageMap::protect(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                         virtual_address, 1, final_flags);
    if (protected_pages.is_err() || protected_pages.unwrap() != 1) {
        return false;
    }

    auto const query = PageMap::query(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                                      virtual_address);
    return query && query.value().phys == physical_address && query.value().flags == final_flags;
}

auto verify_large_mapping_query_includes_page_offset() -> bool {
    alignas(PAGE_SIZE) unsigned char root[PAGE_SIZE] = {};
    PageTablePool pool;
    auto allocator = [&pool](usize size, usize alignment) {
        return pool.allocate(size, alignment);
    };
    auto const virtual_address = 16_mb;
    auto const physical_address = 18_mb;
    auto const flags = MmuFlags::Present | MmuFlags::Writable;

    auto const mapped =
        PageMap::map(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{}, allocator,
                     virtual_address, physical_address, 2_mb / PAGE_SIZE, flags, FRESH);
    if (mapped.is_err()) {
        return false;
    }

    auto const offset = PAGE_SIZE * 7;
    auto const query = PageMap::query(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                                      virtual_address + offset);
    return query && query.value().size == 2_mb && query.value().phys == physical_address + offset;
}

/// Deposits from a fixed pool of tables, each remembered with the owner it was
/// set aside for, and every call counted. |available| caps how many reserve()
/// may hand out.
struct DepositPool {
    static constexpr usize SIZE = 2;

    alignas(PAGE_SIZE) unsigned char tables[SIZE][PAGE_SIZE] = {};
    ours::PhysAddr owners[SIZE] = {};
    bool held[SIZE] = {};
    usize next = 0;
    usize available = SIZE;
    usize reserved = 0;
    usize withdrawn = 0;
    usize discarded = 0;

    auto reserve(ours::PhysAddr const owner) -> bool {
        if (next == available) {
            return false;
        }
        owners[next] = owner;
        held[next] = true;
        next += 1;
        reserved += 1;
        return true;
    }

    auto withdraw(ours::PhysAddr const owner) -> ours::PhysAddr {
        for (usize i = 0; i < next; ++i) {
            if (held[i] && owners[i] == owner) {
                held[i] = false;
                withdrawn += 1;
                return reinterpret_cast<ours::PhysAddr>(tables[i]);
            }
        }
        return 0;
    }

    auto discard(ours::PhysAddr const owner) -> void {
        for (usize i = 0; i < next; ++i) {
            if (held[i] && owners[i] == owner) {
                held[i] = false;
                discarded += 1;
                return;
            }
        }
    }

    auto discard_all(ours::PhysAddr const owner) -> void {
        for (usize i = 0; i < next; ++i) {
            if (held[i] && owners[i] == owner) {
                held[i] = false;
                discarded += 1;
            }
        }
    }

    [[nodiscard]]
    auto nr_held() const -> usize {
        usize count = 0;
        for (usize i = 0; i < next; ++i) {
            if (held[i]) {
                count += 1;
            }
        }
        return count;
    }
};
static_assert(TableDeposits<DepositPool>);

/// A root, its table pool and a deposit pool, with a 2 MiB leaf of |flags|
/// mapped at LEAF_VA to LEAF_PA.
struct LeafFixture {
    static constexpr ours::VirtAddr LEAF_VA = 16_mb;
    static constexpr ours::PhysAddr LEAF_PA = 18_mb;

    alignas(PAGE_SIZE) unsigned char root[PAGE_SIZE] = {};
    PageTablePool pool;
    DepositPool deposits;

    [[nodiscard]]
    auto table() -> ours::PhysAddr {
        return reinterpret_cast<ours::PhysAddr>(root);
    }

    [[nodiscard]]
    auto map_leaf(MmuFlags const flags) -> bool {
        auto allocator = [this](usize size, usize alignment) {
            return pool.allocate(size, alignment);
        };
        return PageMap::map(table(), IdentityPhysToVirt{}, allocator, deposits, LEAF_VA, LEAF_PA,
                            2_mb / PAGE_SIZE, flags, FRESH)
            .is_ok();
    }

    /// What the page |index| of the leaf's range translates to: its frame and
    /// flags, and the size of the entry that maps it.
    [[nodiscard]]
    auto query(usize const index) -> ustl::Option<QueryResult> {
        return PageMap::query(table(), IdentityPhysToVirt{}, LEAF_VA + index * PAGE_SIZE);
    }

    /// Whether every page of the leaf's range but |except| is mapped with a
    /// small entry, to its own frame, with |flags|.
    [[nodiscard]]
    auto small_entries_except(usize const except, MmuFlags const flags) -> bool {
        for (usize i = 0; i < 2_mb / PAGE_SIZE; ++i) {
            if (i == except) {
                continue;
            }
            auto const result = query(i);
            if (!result || result.value().size != PAGE_SIZE ||
                result.value().phys != LEAF_PA + i * PAGE_SIZE || result.value().flags != flags) {
                return false;
            }
        }
        return true;
    }
};

// What a user leaf reads back as: x86 has no read-disable bit, and a leaf
// without Executable is NX.
constexpr auto USER_RW = MmuFlags::Present | MmuFlags::Writable | MmuFlags::User;
constexpr auto USER_RW_READ = USER_RW | MmuFlags::Readable;

/// With deposits, a 2 MiB leaf is mapped with one table set aside for it.
auto verify_leaf_takes_a_deposit() -> bool {
    LeafFixture fixture;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    auto const result = fixture.query(7);
    // Two tables from the allocator: the third and the second level.
    return result && result.value().size == 2_mb &&
           result.value().phys == LeafFixture::LEAF_PA + 7 * PAGE_SIZE &&
           fixture.deposits.reserved == 1 && fixture.deposits.nr_held() == 1 &&
           fixture.pool.next == 2;
}

/// With no deposit to be had, the range is mapped with small entries.
auto verify_no_deposit_maps_small_entries() -> bool {
    LeafFixture fixture;
    fixture.deposits.available = 0;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    return fixture.small_entries_except(2_mb / PAGE_SIZE, USER_RW_READ) &&
           fixture.deposits.nr_held() == 0 && fixture.pool.next == 3;
}

/// An unmap of one page inside the leaf splits it into the deposit: nothing is
/// allocated, the page is gone, and the other 511 are small entries to the same
/// frames with the leaf's flags.
auto verify_unmap_of_part_splits() -> bool {
    LeafFixture fixture;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    auto const tables_before = fixture.pool.next;
    auto const unmapped = PageMap::unmap(fixture.table(), IdentityPhysToVirt{}, fixture.deposits,
                                         LeafFixture::LEAF_VA + 7 * PAGE_SIZE, 1);
    if (unmapped.is_err() || unmapped.unwrap() != 1 || fixture.pool.next != tables_before ||
        fixture.query(7) || !fixture.small_entries_except(7, USER_RW_READ) ||
        fixture.deposits.withdrawn != 1 || fixture.deposits.nr_held() != 0) {
        return false;
    }

    // The split went into the one table set aside. Its entries are rebuilt, not
    // copied from the leaf: bit 7 is PageSize in the leaf and PAT in a small
    // entry, where it would change the memory type. A query cannot see it.
    auto const *const entries = reinterpret_cast<u64 const *>(fixture.deposits.tables[0]);
    for (usize i = 0; i < 2_mb / PAGE_SIZE; ++i) {
        if (i != 7 && (entries[i] & X86_MMUF_PAGE_SIZE) != 0) {
            return false;
        }
    }
    return true;
}

/// A protect of the first page splits the leaf the same way, and only that
/// page loses Write.
auto verify_protect_of_part_splits() -> bool {
    LeafFixture fixture;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    auto const read_only = MmuFlags::Present | MmuFlags::User;
    auto const protected_pages =
        PageMap::protect(fixture.table(), IdentityPhysToVirt{}, fixture.deposits,
                         LeafFixture::LEAF_VA, 1, read_only);
    auto const first = fixture.query(0);
    return protected_pages.is_ok() && protected_pages.unwrap() == 1 && first &&
           first.value().size == PAGE_SIZE &&
           first.value().flags == (read_only | MmuFlags::Readable) &&
           fixture.small_entries_except(0, USER_RW_READ) && fixture.deposits.withdrawn == 1;
}

/// An unmap of the whole leaf clears it and gives its deposit back.
auto verify_unmap_of_whole_leaf_discards() -> bool {
    LeafFixture fixture;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    auto const unmapped = PageMap::unmap(fixture.table(), IdentityPhysToVirt{}, fixture.deposits,
                                         LeafFixture::LEAF_VA, 2_mb / PAGE_SIZE);
    return unmapped.is_ok() && unmapped.unwrap() == 2_mb / PAGE_SIZE && !fixture.query(0) &&
           fixture.deposits.discarded == 1 && fixture.deposits.withdrawn == 0 &&
           fixture.deposits.nr_held() == 0;
}

/// A teardown with the leaf still mapped gives its deposit back with the
/// second-level table that held it.
auto verify_teardown_discards() -> bool {
    LeafFixture fixture;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    auto deallocate = [](ours::PhysAddr, usize) {};
    auto const teardown = PageMap::free_tables(fixture.table(), IdentityPhysToVirt{}, deallocate,
                                               fixture.deposits, 0, 511);
    return teardown.leaves == 1 && teardown.tables == 2 && fixture.deposits.discarded == 1 &&
           fixture.deposits.nr_held() == 0;
}

/// Without deposits nothing changes: a part of a large leaf is InvArgs.
auto verify_no_deposits_refuses_part() -> bool {
    LeafFixture fixture;
    auto allocator = [&fixture](usize size, usize alignment) {
        return fixture.pool.allocate(size, alignment);
    };
    if (PageMap::map(fixture.table(), IdentityPhysToVirt{}, allocator, LeafFixture::LEAF_VA,
                     LeafFixture::LEAF_PA, 2_mb / PAGE_SIZE, USER_RW, FRESH)
            .is_err()) {
        return false;
    }
    auto const unmapped =
        PageMap::unmap(fixture.table(), IdentityPhysToVirt{}, LeafFixture::LEAF_VA + PAGE_SIZE, 1);
    auto const result = fixture.query(1);
    return unmapped.is_err() && unmapped.error() == MapError::InvArgs && result &&
           result.value().size == 2_mb;
}

/// A table pool and a root, for the map option checks.
struct OptionsFixture {
    alignas(PAGE_SIZE) unsigned char root[PAGE_SIZE] = {};
    PageTablePool pool;

    [[nodiscard]]
    auto table() -> ours::PhysAddr {
        return reinterpret_cast<ours::PhysAddr>(root);
    }

    auto map(ours::VirtAddr const va, ours::PhysAddr const pa, usize const n,
             MapOptions const options, usize *const mapped = nullptr)
        -> ustl::Result<void, MapError> {
        auto allocator = [this](usize size, usize alignment) {
            return pool.allocate(size, alignment);
        };
        return PageMap::map(table(), IdentityPhysToVirt{}, allocator, va, pa, n, USER_RW, options,
                            mapped);
    }

    /// The frame |va| translates to, or none.
    [[nodiscard]]
    auto frame(ours::VirtAddr const va) -> ustl::Option<ours::PhysAddr> {
        auto const result = PageMap::query(table(), IdentityPhysToVirt{}, va);
        if (!result) {
            return ustl::NONE;
        }
        return ustl::some(result.value().phys);
    }
};

/// Options that are not one bit of each group are refused before anything
/// is mapped.
auto verify_invalid_options_refused() -> bool {
    constexpr ours::VirtAddr VA = 4_mb;
    OptionsFixture fixture;
    MapOptions const invalid[] = {
        MapOptions{},
        MapOptions::ErrorIfExisting,
        MapOptions::UndoOnFailure,
        MapOptions::ErrorIfExisting | MapOptions::SkipIfExisting | MapOptions::UndoOnFailure,
        MapOptions::ErrorIfExisting | MapOptions::UndoOnFailure | MapOptions::KeepOnFailure,
        MapOptions::SkipIfExisting | MapOptions::OverwriteIfExisting | MapOptions::KeepOnFailure,
    };
    for (auto const options: invalid) {
        auto const result = fixture.map(VA, 6_mb, 1, options);
        if (!result.is_err() || result.error() != MapError::InvArgs) {
            return false;
        }
    }
    return fixture.pool.next == 0 && !fixture.frame(VA);
}

/// Page 2 of four is mapped already. Error stops there; Undo takes pages 0
/// and 1 back, and the page that was there stays.
auto verify_error_undo_leaves_nothing() -> bool {
    constexpr ours::VirtAddr VA = 4_mb;
    OptionsFixture fixture;
    if (fixture.map(VA + 2 * PAGE_SIZE, 10_mb, 1, FRESH).is_err()) {
        return false;
    }
    usize mapped = ~usize{0};
    auto const result = fixture.map(VA, 6_mb, 4, FRESH, &mapped);
    auto const kept = fixture.frame(VA + 2 * PAGE_SIZE);
    return result.is_err() && result.error() == MapError::Existent && mapped == 0 &&
           !fixture.frame(VA) && !fixture.frame(VA + PAGE_SIZE) && kept && *kept == 10_mb &&
           !fixture.frame(VA + 3 * PAGE_SIZE);
}

/// The same with Keep: pages 0 and 1 stay mapped, and the call says so.
auto verify_error_keep_says_how_far() -> bool {
    constexpr ours::VirtAddr VA = 4_mb;
    OptionsFixture fixture;
    if (fixture.map(VA + 2 * PAGE_SIZE, 10_mb, 1, FRESH).is_err()) {
        return false;
    }
    usize mapped = 0;
    auto const result =
        fixture.map(VA, 6_mb, 4, MapOptions::ErrorIfExisting | MapOptions::KeepOnFailure, &mapped);
    auto const first = fixture.frame(VA);
    auto const second = fixture.frame(VA + PAGE_SIZE);
    auto const kept = fixture.frame(VA + 2 * PAGE_SIZE);
    return result.is_err() && result.error() == MapError::Existent && mapped == 2 && first &&
           *first == 6_mb && second && *second == 6_mb + PAGE_SIZE && kept && *kept == 10_mb &&
           !fixture.frame(VA + 3 * PAGE_SIZE);
}

/// Skip maps around the page that is there and leaves it as it was.
auto verify_skip_leaves_existing_page() -> bool {
    constexpr ours::VirtAddr VA = 4_mb;
    OptionsFixture fixture;
    if (fixture.map(VA + 2 * PAGE_SIZE, 10_mb, 1, FRESH).is_err()) {
        return false;
    }
    usize mapped = 0;
    auto const result =
        fixture.map(VA, 6_mb, 4, MapOptions::SkipIfExisting | MapOptions::UndoOnFailure, &mapped);
    auto const first = fixture.frame(VA);
    auto const kept = fixture.frame(VA + 2 * PAGE_SIZE);
    auto const last = fixture.frame(VA + 3 * PAGE_SIZE);
    return result.is_ok() && mapped == 4 && first && *first == 6_mb && kept && *kept == 10_mb &&
           last && *last == 6_mb + 3 * PAGE_SIZE;
}

/// Skip steps over a whole 2 MiB leaf in one go: the page before it and the
/// page after it are mapped, and the leaf is untouched.
auto verify_skip_steps_over_a_leaf() -> bool {
    LeafFixture fixture;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    auto allocator = [&fixture](usize size, usize alignment) {
        return fixture.pool.allocate(size, alignment);
    };
    constexpr auto BEFORE = LeafFixture::LEAF_VA - PAGE_SIZE;
    constexpr auto PAGES = 2_mb / PAGE_SIZE + 2;
    usize mapped = 0;
    auto const result = PageMap::map(
        fixture.table(), IdentityPhysToVirt{}, allocator, fixture.deposits, BEFORE, 30_mb, PAGES,
        USER_RW, MapOptions::SkipIfExisting | MapOptions::UndoOnFailure, &mapped);
    auto const before = PageMap::query(fixture.table(), IdentityPhysToVirt{}, BEFORE);
    auto const after =
        PageMap::query(fixture.table(), IdentityPhysToVirt{}, LeafFixture::LEAF_VA + 2_mb);
    auto const leaf = fixture.query(7);
    return result.is_ok() && mapped == PAGES && before && before.value().phys == 30_mb && after &&
           after.value().phys == 30_mb + (PAGES - 1) * PAGE_SIZE && leaf &&
           leaf.value().size == 2_mb && leaf.value().phys == LeafFixture::LEAF_PA + 7 * PAGE_SIZE;
}

/// A range whose frames are not 2 MiB aligned with its addresses needs a
/// final-level table for each 2 MiB it touches; the pool runs out at the
/// third. VA is one page before a 2 MiB boundary, so the call gets through
/// that page and the next 512 before it fails.
constexpr ours::VirtAddr NOMEM_VA = 2_mb - PAGE_SIZE;
constexpr ours::PhysAddr NOMEM_PA = PAGE_SIZE;
constexpr usize NOMEM_PAGES = 1 + 3 * (2_mb / PAGE_SIZE);

/// Undo after running out takes back the 513 pages it had mapped.
auto verify_nomem_undo_leaves_nothing() -> bool {
    OptionsFixture fixture;
    usize mapped = ~usize{0};
    auto const result = fixture.map(NOMEM_VA, NOMEM_PA, NOMEM_PAGES, FRESH, &mapped);
    return result.is_err() && result.error() == MapError::NoMem && mapped == 0 &&
           !fixture.frame(NOMEM_VA) && !fixture.frame(2_mb + 5 * PAGE_SIZE);
}

/// Keep after running out leaves them, and says it got through 513.
auto verify_nomem_keep_says_how_far() -> bool {
    OptionsFixture fixture;
    usize mapped = 0;
    auto const result =
        fixture.map(NOMEM_VA, NOMEM_PA, NOMEM_PAGES,
                    MapOptions::ErrorIfExisting | MapOptions::KeepOnFailure, &mapped);
    auto const inside = fixture.frame(2_mb + 5 * PAGE_SIZE);
    return result.is_err() && result.error() == MapError::NoMem && mapped == 1 + 2_mb / PAGE_SIZE &&
           fixture.frame(NOMEM_VA) && inside && *inside == NOMEM_PA + 6 * PAGE_SIZE &&
           !fixture.frame(4_mb);
}

constexpr auto OVERWRITE = MapOptions::OverwriteIfExisting | MapOptions::UndoOnFailure;

/// Overwrite writes a page over with another frame and other permissions.
auto verify_overwrite_replaces_a_page() -> bool {
    constexpr ours::VirtAddr VA = 4_mb;
    constexpr auto READ_ONLY = MmuFlags::Present | MmuFlags::User;
    OptionsFixture fixture;
    auto allocator = [&fixture](usize size, usize alignment) {
        return fixture.pool.allocate(size, alignment);
    };
    if (PageMap::map(fixture.table(), IdentityPhysToVirt{}, allocator, VA, 6_mb, 1, READ_ONLY,
                     FRESH)
            .is_err()) {
        return false;
    }
    usize mapped = 0;
    auto const result = fixture.map(VA, 10_mb, 1, OVERWRITE, &mapped);
    auto const after = PageMap::query(fixture.table(), IdentityPhysToVirt{}, VA);
    return result.is_ok() && mapped == 1 && after && after.value().phys == 10_mb &&
           after.value().flags == USER_RW_READ;
}

/// Overwrite of one page inside a 2 MiB leaf splits the leaf into its
/// deposit, writes that page over, and leaves the other 511 on their frames.
auto verify_overwrite_inside_a_leaf_splits() -> bool {
    LeafFixture fixture;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    auto allocator = [&fixture](usize size, usize alignment) {
        return fixture.pool.allocate(size, alignment);
    };
    auto const tables_before = fixture.pool.next;
    auto const result =
        PageMap::map(fixture.table(), IdentityPhysToVirt{}, allocator, fixture.deposits,
                     LeafFixture::LEAF_VA + 7 * PAGE_SIZE, 30_mb, 1, USER_RW, OVERWRITE);
    auto const page = fixture.query(7);
    return result.is_ok() && page && page.value().size == PAGE_SIZE && page.value().phys == 30_mb &&
           fixture.small_entries_except(7, USER_RW_READ) && fixture.deposits.withdrawn == 1 &&
           fixture.pool.next == tables_before;
}

/// Overwrite of a whole 2 MiB leaf with a 2 MiB aligned run writes the leaf
/// over in place: still one leaf, on the new frames, its deposit kept.
auto verify_overwrite_of_a_whole_leaf_stays_a_leaf() -> bool {
    LeafFixture fixture;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    auto allocator = [&fixture](usize size, usize alignment) {
        return fixture.pool.allocate(size, alignment);
    };
    auto const result =
        PageMap::map(fixture.table(), IdentityPhysToVirt{}, allocator, fixture.deposits,
                     LeafFixture::LEAF_VA, 32_mb, 2_mb / PAGE_SIZE, USER_RW, OVERWRITE);
    auto const page = fixture.query(7);
    return result.is_ok() && page && page.value().size == 2_mb &&
           page.value().phys == 32_mb + 7 * PAGE_SIZE && fixture.deposits.nr_held() == 1 &&
           fixture.deposits.withdrawn == 0 && fixture.deposits.reserved == 1;
}

/// A 2 MiB aligned run written over a table of small entries goes into the
/// table and writes each entry over: no leaf replaces the table.
auto verify_overwrite_over_small_entries_keeps_the_table() -> bool {
    LeafFixture fixture;
    fixture.deposits.available = 0;
    if (!fixture.map_leaf(USER_RW)) {
        return false;
    }
    fixture.deposits.available = DepositPool::SIZE;
    auto allocator = [&fixture](usize size, usize alignment) {
        return fixture.pool.allocate(size, alignment);
    };
    auto const result =
        PageMap::map(fixture.table(), IdentityPhysToVirt{}, allocator, fixture.deposits,
                     LeafFixture::LEAF_VA, 32_mb, 2_mb / PAGE_SIZE, USER_RW, OVERWRITE);
    auto const first = fixture.query(0);
    auto const last = fixture.query(2_mb / PAGE_SIZE - 1);
    return result.is_ok() && first && first.value().size == PAGE_SIZE &&
           first.value().phys == 32_mb && last && last.value().phys == 32_mb + 2_mb - PAGE_SIZE &&
           fixture.deposits.reserved == 0;
}

/// Without deposits a leaf covered in part cannot be split, so Overwrite of a
/// page inside it is InvArgs, and the leaf stays as it was.
auto verify_overwrite_without_deposits_refuses_part() -> bool {
    LeafFixture fixture;
    auto allocator = [&fixture](usize size, usize alignment) {
        return fixture.pool.allocate(size, alignment);
    };
    if (PageMap::map(fixture.table(), IdentityPhysToVirt{}, allocator, LeafFixture::LEAF_VA,
                     LeafFixture::LEAF_PA, 2_mb / PAGE_SIZE, USER_RW, FRESH)
            .is_err()) {
        return false;
    }
    auto const result =
        PageMap::map(fixture.table(), IdentityPhysToVirt{}, allocator,
                     LeafFixture::LEAF_VA + PAGE_SIZE, 30_mb, 1, USER_RW, OVERWRITE);
    auto const leaf = fixture.query(1);
    return result.is_err() && result.error() == MapError::InvArgs && leaf &&
           leaf.value().size == 2_mb && leaf.value().phys == LeafFixture::LEAF_PA + PAGE_SIZE;
}

/// An unmap that starts inside one 2 MiB leaf and ends inside the next
/// splits both, as the walk meets each, and takes only the pages between:
/// the two ends are the only places a range can cover a leaf in part.
auto verify_unmap_across_two_leaves_splits_both() -> bool {
    LeafFixture fixture;
    auto allocator = [&fixture](usize size, usize alignment) {
        return fixture.pool.allocate(size, alignment);
    };
    constexpr usize LEAF_PAGES = 2_mb / PAGE_SIZE;
    if (PageMap::map(fixture.table(), IdentityPhysToVirt{}, allocator, fixture.deposits,
                     LeafFixture::LEAF_VA, LeafFixture::LEAF_PA, 2 * LEAF_PAGES, USER_RW, FRESH)
            .is_err()) {
        return false;
    }
    auto const unmapped = PageMap::unmap(fixture.table(), IdentityPhysToVirt{}, fixture.deposits,
                                         LeafFixture::LEAF_VA + 7 * PAGE_SIZE, LEAF_PAGES);
    auto const before = fixture.query(6);
    auto const after = fixture.query(LEAF_PAGES + 7);
    return unmapped.is_ok() && unmapped.unwrap() == LEAF_PAGES && before &&
           before.value().size == PAGE_SIZE &&
           before.value().phys == LeafFixture::LEAF_PA + 6 * PAGE_SIZE && !fixture.query(7) &&
           !fixture.query(LEAF_PAGES + 6) && after && after.value().size == PAGE_SIZE &&
           after.value().phys == LeafFixture::LEAF_PA + (LEAF_PAGES + 7) * PAGE_SIZE &&
           fixture.deposits.withdrawn == 2 && fixture.deposits.nr_held() == 0;
}

/// map_frames() of frames that are not contiguous maps each page to its own
/// frame, in one call.
auto verify_map_frames_maps_each_page() -> bool {
    constexpr ours::VirtAddr VA = 4_mb;
    OptionsFixture fixture;
    auto allocator = [&fixture](usize size, usize alignment) {
        return fixture.pool.allocate(size, alignment);
    };
    ours::PhysAddr frames[] = {10_mb, 20_mb, 6_mb, 30_mb};
    usize mapped = 0;
    auto const result = PageMap::map_frames(fixture.table(), IdentityPhysToVirt{}, allocator,
                                            NoDeposits{}, VA, frames, 4, USER_RW, FRESH, &mapped);
    if (result.is_err() || mapped != 4) {
        return false;
    }
    for (usize i = 0; i < 4; ++i) {
        auto const frame = fixture.frame(VA + i * PAGE_SIZE);
        if (!frame || *frame != frames[i]) {
            return false;
        }
    }
    return true;
}

/// map_frames() over a page that is there: Error with Undo leaves nothing of
/// the call, Error with Keep says it got through two, and Skip maps around it.
auto verify_map_frames_honours_its_options() -> bool {
    constexpr ours::VirtAddr VA = 4_mb;
    ours::PhysAddr frames[] = {10_mb, 20_mb, 6_mb, 30_mb};
    auto const run = [&](MapOptions const options, usize *mapped, OptionsFixture &fixture) {
        auto allocator = [&fixture](usize size, usize alignment) {
            return fixture.pool.allocate(size, alignment);
        };
        if (fixture.map(VA + 2 * PAGE_SIZE, 40_mb, 1, FRESH).is_err()) {
            return ustl::Result<void, MapError>(ustl::err(MapError::NoMem));
        }
        return PageMap::map_frames(fixture.table(), IdentityPhysToVirt{}, allocator, NoDeposits{},
                                   VA, frames, 4, USER_RW, options, mapped);
    };

    OptionsFixture undo;
    usize undone = ~usize{0};
    auto const failed = run(FRESH, &undone, undo);
    auto const kept_there = undo.frame(VA + 2 * PAGE_SIZE);
    if (!failed.is_err() || failed.error() != MapError::Existent || undone != 0 || undo.frame(VA) ||
        !kept_there || *kept_there != 40_mb) {
        return false;
    }

    OptionsFixture keep;
    usize got = 0;
    auto const kept = run(MapOptions::ErrorIfExisting | MapOptions::KeepOnFailure, &got, keep);
    auto const second = keep.frame(VA + PAGE_SIZE);
    if (!kept.is_err() || got != 2 || !second || *second != 20_mb ||
        keep.frame(VA + 3 * PAGE_SIZE)) {
        return false;
    }

    OptionsFixture skip;
    usize all = 0;
    auto const skipped = run(MapOptions::SkipIfExisting | MapOptions::UndoOnFailure, &all, skip);
    auto const left = skip.frame(VA + 2 * PAGE_SIZE);
    auto const last = skip.frame(VA + 3 * PAGE_SIZE);
    return skipped.is_ok() && all == 4 && left && *left == 40_mb && last && *last == 30_mb;
}
} // namespace

auto main() -> int {
    if (!verify_mapping(2_mb, 2_mb, 1, PAGE_SIZE)) {
        return 1;
    }
    if (!verify_mapping(2_mb, 2_mb, 2_mb / PAGE_SIZE, 2_mb)) {
        return 2;
    }
    if (!verify_mapping(0, 0, 1, PAGE_SIZE)) {
        return 3;
    }
    if (!verify_empty_and_invalid_ranges()) {
        return 4;
    }
    if (!verify_unmapping_removes_an_existing_mapping()) {
        return 5;
    }
    if (!verify_protection_updates_an_existing_mapping()) {
        return 6;
    }
    if (!verify_large_mapping_query_includes_page_offset()) {
        return 7;
    }
    if (!verify_leaf_takes_a_deposit()) {
        return 8;
    }
    if (!verify_no_deposit_maps_small_entries()) {
        return 9;
    }
    if (!verify_unmap_of_part_splits()) {
        return 10;
    }
    if (!verify_protect_of_part_splits()) {
        return 11;
    }
    if (!verify_unmap_of_whole_leaf_discards()) {
        return 12;
    }
    if (!verify_teardown_discards()) {
        return 13;
    }
    if (!verify_no_deposits_refuses_part()) {
        return 14;
    }
    if (!verify_invalid_options_refused()) {
        return 15;
    }
    if (!verify_error_undo_leaves_nothing()) {
        return 16;
    }
    if (!verify_error_keep_says_how_far()) {
        return 17;
    }
    if (!verify_skip_leaves_existing_page()) {
        return 18;
    }
    if (!verify_skip_steps_over_a_leaf()) {
        return 19;
    }
    if (!verify_nomem_undo_leaves_nothing()) {
        return 20;
    }
    if (!verify_nomem_keep_says_how_far()) {
        return 21;
    }
    if (!verify_overwrite_replaces_a_page()) {
        return 22;
    }
    if (!verify_overwrite_inside_a_leaf_splits()) {
        return 23;
    }
    if (!verify_overwrite_of_a_whole_leaf_stays_a_leaf()) {
        return 24;
    }
    if (!verify_overwrite_over_small_entries_keeps_the_table()) {
        return 25;
    }
    if (!verify_overwrite_without_deposits_refuses_part()) {
        return 26;
    }
    if (!verify_unmap_across_two_leaves_splits_both()) {
        return 27;
    }
    if (!verify_map_frames_maps_each_page()) {
        return 28;
    }
    if (!verify_map_frames_honours_its_options()) {
        return 29;
    }
    return 0;
}
