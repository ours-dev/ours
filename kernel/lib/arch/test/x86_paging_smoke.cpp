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
                               allocator, virtual_address, physical_address, pages, flags);
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
                              allocator, 0, 0, 0, flags);
    if (empty.is_err() || pool.next != 0) {
        return false;
    }
    auto unaligned = PageMap::map(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                                  allocator, 1, 0, 1, flags);
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
                                     allocator, virtual_address, physical_address, 1, flags);
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
                     virtual_address, physical_address, 1, initial_flags);
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
                     virtual_address, physical_address, 2_mb / PAGE_SIZE, flags);
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
                            2_mb / PAGE_SIZE, flags)
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
                     LeafFixture::LEAF_PA, 2_mb / PAGE_SIZE, USER_RW)
            .is_err()) {
        return false;
    }
    auto const unmapped =
        PageMap::unmap(fixture.table(), IdentityPhysToVirt{}, LeafFixture::LEAF_VA + PAGE_SIZE, 1);
    auto const result = fixture.query(1);
    return unmapped.is_err() && unmapped.error() == MapError::InvArgs && result &&
           result.value().size == 2_mb;
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
    return 0;
}
