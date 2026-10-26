/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <page_tables/mmu_format.hpp>
#include <page_tables/page_tables.hpp>

#include <array>
#include <deque>
#include <map>
#include <vector>

#include <arch/paging.hpp>
#include <gtest/gtest.h>

namespace {
using arch::paging::MmuFlags;
using ours::Status;
using page_tables::Mapping;
using page_tables::MmuFormat;
using page_tables::PageTableLevel;
using page_tables::PhysAddr;
using page_tables::PteVal;
using page_tables::VirtAddr;

constexpr usize KB = 1024;
constexpr usize MB = 1024 * KB;
constexpr usize GB = 1024 * MB;

constexpr auto RWX = MmuFlags::Readable | MmuFlags::Writable | MmuFlags::Executable;
constexpr auto USER_RWX = RWX | MmuFlags::User;

/// A table as the hardware sees it: 512 entries, page aligned.
struct alignas(4 * KB) Table {
    std::array<PteVal, X86_MAX_PTES> entries{};
};

/// Every table made for a test, so that a table's address stays valid for
/// as long as the test runs.
class Arena {
  public:
    auto make() -> PhysAddr {
        return reinterpret_cast<PhysAddr>(m_tables.emplace_back().entries.data());
    }

  private:
    std::deque<Table> m_tables;
};

/// Tables set aside for 2 MiB leaves, by the table that holds the leaf: the
/// arch::paging::TableDeposits the paging engine takes, and what HostTable's
/// deposit hooks forward to, so that the two can be run on the same tables.
class HostDeposits {
  public:
    explicit HostDeposits(Arena *arena) : m_arena(arena) {}

    auto reserve(PhysAddr const owner) -> bool {
        m_held[owner].push_back(m_arena->make());
        return true;
    }

    auto withdraw(PhysAddr const owner) -> PhysAddr {
        auto &held = m_held[owner];
        if (held.empty()) {
            ADD_FAILURE() << "no table set aside with " << std::hex << owner;
            return m_arena->make();
        }
        auto const table = held.back();
        held.pop_back();
        return table;
    }

    auto discard(PhysAddr const owner) -> void {
        auto _ = withdraw(owner);
    }

    auto discard_all(PhysAddr const owner) -> void {
        m_held.erase(owner);
    }

    [[nodiscard]]
    auto held() const -> usize {
        usize count = 0;
        for (auto const &[owner, tables]: m_held) {
            count += tables.size();
        }
        return count;
    }

  private:
    Arena *m_arena;
    std::map<PhysAddr, std::vector<PhysAddr>> m_held;
};
static_assert(arch::paging::TableDeposits<HostDeposits>);

/// A page table in host memory, whose physical addresses are the host's own:
/// a table's address is where it is. The walks' contract, filled in the
/// simplest way it can be, with the kernel's own entry format.
class HostTable final: public page_tables::X86PageTableImpl<HostTable> {
    friend class page_tables::X86PageTableImpl<HostTable>;

  public:
    explicit HostTable(bool const splits = false)
        : m_deposits(&m_arena), m_root(m_arena.make()), m_splits(splits) {}

    using X86PageTableImpl::free_tables;
    using X86PageTableImpl::map;
    using X86PageTableImpl::map_frames;
    using X86PageTableImpl::protect;
    using X86PageTableImpl::query;
    using X86PageTableImpl::second_level_empty;
    using X86PageTableImpl::unmap;

    /// Lets |count| more tables be allocated, and fails every one after.
    auto fail_allocations_after(usize const count) -> void {
        m_allocations_left = count;
    }

    /// What the paging engine's map takes as its allocator: the same tables,
    /// failing when this table's allocations do.
    [[nodiscard]]
    auto allocator() {
        return [this](usize, usize) { return allocate_table(); };
    }

    /// What the paging engine's free_tables() takes to give a table back.
    [[nodiscard]]
    auto deallocator() {
        return [this](PhysAddr const pa, usize) { free_table(pa); };
    }

    /// The tables given back so far.
    [[nodiscard]]
    auto freed() const -> usize {
        return m_freed;
    }

    /// The root's entry at |at|.
    [[nodiscard]]
    auto root_entry(usize const at) const -> PteVal {
        return table(m_root)[at].value;
    }

    /// The table at |level| that |va| falls under, with a table made on the
    /// way down for every level above it that has none.
    auto owner(VirtAddr const va, PageTableLevel const level) -> PhysAddr {
        auto at = top_level();
        auto pa = m_root;
        while (at != level) {
            auto &slot = table(pa)[page_tables::index(at, va)].value;
            if (!page_tables::is_present(slot)) {
                slot = MmuFormat::table_entry(m_arena.make());
            }
            pa = page_tables::address(slot);
            at = page_tables::lower(at);
        }
        return pa;
    }

    /// Writes a leaf at |level| mapping |va| to |pa| with |flags|, as a map
    /// would have: a 2 MiB one in a table that splits its large leaves with
    /// a table set aside for it.
    auto leaf(VirtAddr const va, PageTableLevel const level, PhysAddr const pa,
              MmuFlags const flags) -> void {
        auto const in = owner(va, level);
        table(in)[page_tables::index(level, va)].value =
            MmuFormat::leaf(pa, MmuFormat::leaf_bits(level, flags));
        if (level == PageTableLevel::PageDirectory && m_splits) {
            auto _ = m_deposits.reserve(in);
        }
    }

    /// 4 KiB leaves for |n| pages from |va|, to frames from |pa| on.
    auto small_leaves(VirtAddr const va, PhysAddr const pa, usize const n, MmuFlags const flags)
        -> void {
        for (usize i = 0; i < n; ++i) {
            leaf(va + i * PAGE_SIZE, PageTableLevel::PageTable, pa + i * PAGE_SIZE, flags);
        }
    }

    /// The raw entry of the leaf that maps |va|, or 0.
    [[nodiscard]]
    auto leaf_entry(VirtAddr const va) const -> PteVal {
        auto level = top_level();
        auto pa = m_root;
        for (;;) {
            auto const entry = table(pa)[page_tables::index(level, va)].value;
            if (!page_tables::is_present(entry)) {
                return 0;
            }
            if (page_tables::is_leaf(level, entry)) {
                return entry;
            }
            pa = page_tables::address(entry);
            level = page_tables::lower(level);
        }
    }

    [[nodiscard]]
    auto root_phys() const -> PhysAddr {
        return m_root;
    }

    [[nodiscard]]
    auto deposits() -> HostDeposits & {
        return m_deposits;
    }

  private:
    using Format = MmuFormat;

    static constexpr auto top_level() -> PageTableLevel {
        return PageTableLevel::PageMapLevel4;
    }

    [[nodiscard]]
    auto root() const -> PhysAddr {
        return m_root;
    }

    [[nodiscard]]
    static auto table(PhysAddr const pa) -> page_tables::Pte * {
        return reinterpret_cast<page_tables::Pte *>(pa);
    }

    [[nodiscard]]
    auto splits_large_leaves() const -> bool {
        return m_splits;
    }

    auto withdraw_table(PhysAddr const owner) -> PhysAddr {
        return m_deposits.withdraw(owner);
    }

    auto discard_table(PhysAddr const owner) -> void {
        m_deposits.discard(owner);
    }

    auto reserve_table(PhysAddr const owner) -> bool {
        return m_deposits.reserve(owner);
    }

    auto discard_all_tables(PhysAddr const owner) -> void {
        m_deposits.discard_all(owner);
    }

    auto free_table(PhysAddr) -> void {
        m_freed += 1;
    }

    auto allocate_table() -> PhysAddr {
        if (m_allocations_left == 0) {
            return 0;
        }
        m_allocations_left -= 1;
        return m_arena.make();
    }

    Arena m_arena;
    HostDeposits m_deposits;
    PhysAddr m_root;
    bool m_splits;
    usize m_allocations_left = ~usize{0};
    usize m_freed = 0;
};

using Engine =
    arch::paging::Paging<arch::paging::PagingDispatcher<PageTableLevel::PageMapLevel4>::Type>;

auto identity(PhysAddr const pa) -> VirtAddr {
    return pa;
}

/// What the kernel's PageTable made of the engine's result.
template <typename T>
auto status_of(ustl::Result<T, arch::paging::MapError> const &result) -> Status {
    if (result.is_ok()) {
        return Status::Ok;
    }
    switch (result.error()) {
        case arch::paging::MapError::InvArgs:
            return Status::InvArgs;
        case arch::paging::MapError::Existent:
            return Status::AlreadyExists;
        case arch::paging::MapError::NoMem:
            return Status::NoMemory;
    }
    return Status::InternalError;
}

TEST(Query, NothingMappedIsNone) {
    HostTable table;
    EXPECT_TRUE(table.query(0x40'0000).is_none());
}

TEST(Query, AFourKibLeafTranslatesTheOffsetWithinIt) {
    HostTable table;
    table.leaf(0x40'1000, PageTableLevel::PageTable, 0x1234'5000, USER_RWX);

    auto const found = table.query(0x40'1abc);
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(found.value().phys, 0x1234'5abcU);
    EXPECT_EQ(found.value().size, 4 * KB);
    EXPECT_EQ(found.value().flags, MmuFlags::Present | USER_RWX);
}

TEST(Query, ANeighbourInTheSameTableIsNone) {
    HostTable table;
    table.leaf(0x40'1000, PageTableLevel::PageTable, 0x1234'5000, USER_RWX);
    EXPECT_TRUE(table.query(0x40'2000).is_none());
}

TEST(Query, LargeLeavesTranslateTheOffsetWithinThem) {
    HostTable table;
    table.leaf(0x20'0000, PageTableLevel::PageDirectory, 0x8000'0000, MmuFlags::Readable);
    table.leaf(0x80'0000'0000, PageTableLevel::PageDirectoryPointerTable, 0x1'0000'0000, RWX);

    auto const two_mib = table.query(0x20'0000 + 0x1'2345);
    ASSERT_TRUE(two_mib.is_some());
    EXPECT_EQ(two_mib.value().phys, 0x8000'0000U + 0x1'2345);
    EXPECT_EQ(two_mib.value().size, 2 * MB);
    EXPECT_EQ(two_mib.value().flags, MmuFlags::Present | MmuFlags::Readable);

    auto const one_gib = table.query(0x80'0000'0000 + 0x1234'5678);
    ASSERT_TRUE(one_gib.is_some());
    EXPECT_EQ(one_gib.value().phys, 0x1'0000'0000U + 0x1234'5678);
    EXPECT_EQ(one_gib.value().size, 1 * GB);
}

/// The same tables, read by the paging engine's query and by this one, give
/// the same answer at every address tried: what the kernel's query_mapping()
/// returned before it moved here is what it returns now.
TEST(Query, AgreesWithTheEngine) {
    HostTable table;
    table.leaf(0x40'0000, PageTableLevel::PageTable, 0x10'0000, USER_RWX);
    table.leaf(0x40'1000, PageTableLevel::PageTable, 0x30'0000, MmuFlags::User);
    table.leaf(0x60'0000, PageTableLevel::PageDirectory, 0x4000'0000, RWX);
    table.leaf(0xc0'0000'0000, PageTableLevel::PageDirectoryPointerTable, 0x8000'0000, {});

    for (VirtAddr const va:
         {0x0UL, 0x40'0000UL, 0x40'0fffUL, 0x40'1800UL, 0x40'2000UL, 0x60'0000UL, 0x7f'ffffUL,
          0x80'0000UL, 0xc0'0000'0000UL, 0xc0'3fff'ffffUL, 0xc0'4000'0000UL}) {
        auto const engine = Engine::query(table.root_phys(), identity, va);
        auto const ours = table.query(va);
        ASSERT_EQ(engine.is_some(), ours.is_some()) << std::hex << va;
        if (ours.is_some()) {
            EXPECT_EQ(engine.value().phys, ours.value().phys) << std::hex << va;
            EXPECT_EQ(engine.value().size, ours.value().size) << std::hex << va;
            EXPECT_EQ(engine.value().flags, ours.value().flags) << std::hex << va;
        }
    }
}

TEST(Unmap, ClearsTheRangeAndNothingElse) {
    HostTable table;
    table.small_leaves(0x40'0000, 0x10'0000, 8, USER_RWX);

    EXPECT_EQ(table.unmap(0x40'2000, 4), Status::Ok);
    for (usize i = 0; i < 8; ++i) {
        auto const mapped = table.query(0x40'0000 + i * PAGE_SIZE).is_some();
        EXPECT_EQ(mapped, i < 2 || i >= 6) << i;
    }
}

TEST(Unmap, CrossesATableBoundary) {
    HostTable table;
    table.small_leaves(0x1f'e000, 0x10'0000, 4, USER_RWX);

    EXPECT_EQ(table.unmap(0x1f'f000, 2), Status::Ok);
    EXPECT_TRUE(table.query(0x1f'e000).is_some());
    EXPECT_TRUE(table.query(0x1f'f000).is_none());
    EXPECT_TRUE(table.query(0x20'0000).is_none());
    EXPECT_TRUE(table.query(0x20'1000).is_some());
}

TEST(Unmap, AHoleIsPassedOver) {
    HostTable table;
    table.leaf(0x7f'f000, PageTableLevel::PageTable, 0x10'0000, USER_RWX);
    EXPECT_EQ(table.unmap(0, 0x800), Status::Ok);
    EXPECT_TRUE(table.query(0x7f'f000).is_none());
}

TEST(Unmap, AWholeLargeLeafGivesItsTableBack) {
    HostTable table(true);
    table.leaf(0x60'0000, PageTableLevel::PageDirectory, 0x4000'0000, USER_RWX);
    ASSERT_EQ(table.deposits().held(), 1U);

    EXPECT_EQ(table.unmap(0x60'0000, 512), Status::Ok);
    EXPECT_TRUE(table.query(0x60'0000).is_none());
    EXPECT_EQ(table.deposits().held(), 0U);
}

TEST(Unmap, PartOfALargeLeafSplitsItIntoItsTable) {
    HostTable table(true);
    table.leaf(0x60'0000, PageTableLevel::PageDirectory, 0x4000'0000, USER_RWX);

    EXPECT_EQ(table.unmap(0x60'1000, 1), Status::Ok);
    EXPECT_EQ(table.deposits().held(), 0U);
    EXPECT_TRUE(table.query(0x60'1000).is_none());
    for (VirtAddr const va: {0x60'0000UL, 0x60'2000UL, 0x7f'f000UL}) {
        auto const found = table.query(va);
        ASSERT_TRUE(found.is_some()) << std::hex << va;
        EXPECT_EQ(found.value().phys, 0x4000'0000U + (va - 0x60'0000));
        EXPECT_EQ(found.value().size, 4 * KB);
        EXPECT_EQ(found.value().flags, MmuFlags::Present | USER_RWX);
    }
}

TEST(Unmap, PartOfALargeLeafWithoutDepositsIsInvArgs) {
    HostTable table;
    table.leaf(0x60'0000, PageTableLevel::PageDirectory, 0x4000'0000, RWX);

    EXPECT_EQ(table.unmap(0x60'1000, 1), Status::InvArgs);
    auto const found = table.query(0x60'1000);
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(found.value().size, 2 * MB);
}

TEST(Unmap, ARangeThatIsNotAlignedOrWrapsIsInvArgs) {
    HostTable table;
    EXPECT_EQ(table.unmap(0x40'0800, 1), Status::InvArgs);
    EXPECT_EQ(table.unmap(~VirtAddr{0} - 0xfff, 2), Status::InvArgs);
    EXPECT_EQ(table.unmap(0x40'0000, 0), Status::Ok);
}

TEST(Protect, TakesPermissionsAwayAndNeverGivesOne) {
    HostTable table;
    table.leaf(0x40'0000, PageTableLevel::PageTable, 0x10'0000, USER_RWX);
    table.leaf(0x40'1000, PageTableLevel::PageTable, 0x11'0000,
               MmuFlags::User | MmuFlags::Readable);

    EXPECT_EQ(table.protect(0x40'0000, 2, MmuFlags::Readable | MmuFlags::Writable), Status::Ok);
    EXPECT_EQ(table.query(0x40'0000).value().flags,
              MmuFlags::Present | MmuFlags::User | MmuFlags::Readable | MmuFlags::Writable);
    EXPECT_EQ(table.query(0x40'1000).value().flags,
              MmuFlags::Present | MmuFlags::User | MmuFlags::Readable);
}

TEST(Protect, PartOfALargeLeafSplitsItAndNarrowsOnlyThatPart) {
    HostTable table(true);
    table.leaf(0x60'0000, PageTableLevel::PageDirectory, 0x4000'0000, USER_RWX);

    EXPECT_EQ(table.protect(0x7f'f000, 1, MmuFlags::Readable), Status::Ok);
    EXPECT_EQ(table.query(0x7f'f000).value().flags,
              MmuFlags::Present | MmuFlags::User | MmuFlags::Readable);
    EXPECT_EQ(table.query(0x7f'e000).value().flags, MmuFlags::Present | USER_RWX);
    EXPECT_EQ(table.query(0x7f'e000).value().size, 4 * KB);
}

/// The same tables and the same calls, one run by the paging engine and one
/// by the walks here, end the same: every page of the region maps the same
/// entry, bit for bit, the calls answer the same, and the same tables are
/// set aside. What the kernel's unmap and protect did before they moved here
/// is what they do now.
TEST(UnmapAndProtect, AgreeWithTheEngine) {
    for (bool const splits: {false, true}) {
        HostTable engine(splits);
        HostTable ours(splits);
        for (auto *table: {&engine, &ours}) {
            table->small_leaves(0x40'0000, 0x10'0000, 32, USER_RWX);
            table->leaf(0x60'0000, PageTableLevel::PageDirectory, 0x4000'0000, USER_RWX);
            table->leaf(0x80'0000, PageTableLevel::PageDirectory, 0x4020'0000, RWX);
            table->leaf(0xa0'0000, PageTableLevel::PageDirectory, 0x4040'0000, USER_RWX);
        }

        struct Call {
            bool unmap;
            VirtAddr va;
            usize n;
            MmuFlags flags;
        };
        auto const calls = std::array{
            Call{.unmap = true, .va = 0x40'3000, .n = 4, .flags = {}},
            Call{.unmap = false, .va = 0x40'f000, .n = 0x200, .flags = MmuFlags::Readable},
            Call{.unmap = true, .va = 0x80'1000, .n = 2, .flags = {}},
            Call{.unmap = true, .va = 0xa0'0000, .n = 512, .flags = {}},
            Call{.unmap = false, .va = 0x7f'0000, .n = 0x20, .flags = MmuFlags::Executable},
            Call{.unmap = true, .va = 0x3f'f000, .n = 0x300, .flags = {}},
        };
        for (auto const &call: calls) {
            auto const expected = [&] {
                if (call.unmap && splits) {
                    return status_of(Engine::unmap(engine.root_phys(), identity, engine.deposits(),
                                                   call.va, call.n));
                }
                if (call.unmap) {
                    return status_of(Engine::unmap(engine.root_phys(), identity, call.va, call.n));
                }
                if (splits) {
                    return status_of(Engine::protect(engine.root_phys(), identity,
                                                     engine.deposits(), call.va, call.n,
                                                     call.flags));
                }
                return status_of(
                    Engine::protect(engine.root_phys(), identity, call.va, call.n, call.flags));
            }();
            auto const actual = [&] {
                if (call.unmap) {
                    return ours.unmap(call.va, call.n);
                }
                return ours.protect(call.va, call.n, call.flags);
            }();
            EXPECT_EQ(expected, actual) << std::hex << call.va << " splits " << splits;
        }

        for (VirtAddr va = 0x3f'0000; va < 0xc0'0000; va += PAGE_SIZE) {
            ASSERT_EQ(engine.leaf_entry(va), ours.leaf_entry(va))
                << std::hex << va << " splits " << splits;
        }
        EXPECT_EQ(engine.deposits().held(), ours.deposits().held()) << "splits " << splits;
    }
}

constexpr auto ERROR_UNDO =
    arch::paging::MapOptions::ErrorIfExisting | arch::paging::MapOptions::UndoOnFailure;
constexpr auto ERROR_KEEP =
    arch::paging::MapOptions::ErrorIfExisting | arch::paging::MapOptions::KeepOnFailure;
constexpr auto SKIP_UNDO =
    arch::paging::MapOptions::SkipIfExisting | arch::paging::MapOptions::UndoOnFailure;
constexpr auto OVERWRITE_UNDO =
    arch::paging::MapOptions::OverwriteIfExisting | arch::paging::MapOptions::UndoOnFailure;

/// The size of the leaf that maps |va|, or 0.
auto leaf_size(HostTable const &table, VirtAddr const va) -> usize {
    auto const found = table.query(va);
    if (found.is_none()) {
        return 0;
    }
    return found.value().size;
}

/// The physical address |va| translates to, or 0.
auto phys(HostTable const &table, VirtAddr const va) -> PhysAddr {
    auto const found = table.query(va);
    if (found.is_none()) {
        return 0;
    }
    return found.value().phys;
}

TEST(Map, TakesTheLargestLeafEachStepAllows) {
    HostTable table;
    usize mapped = 0;
    // From a page before a 2 MiB boundary to a page after the next but one,
    // with the frames at the same offset from a 2 MiB boundary.
    EXPECT_EQ(table.map(0x3f'f000, 0x13f'f000, 1 + 1024 + 1, USER_RWX, ERROR_UNDO, &mapped),
              Status::Ok);
    EXPECT_EQ(mapped, 1026U);
    EXPECT_EQ(leaf_size(table, 0x3f'f000), 4 * KB);
    EXPECT_EQ(leaf_size(table, 0x40'0000), 2 * MB);
    EXPECT_EQ(leaf_size(table, 0x60'0000), 2 * MB);
    EXPECT_EQ(leaf_size(table, 0x80'0000), 4 * KB);
    EXPECT_EQ(phys(table, 0x61'2345), 0x161'2345U);
    EXPECT_EQ(table.query(0x80'0000).value().flags, MmuFlags::Present | USER_RWX);
}

TEST(Map, AGibRangeIsOneLeafWithoutDepositsAndTwoMibLeavesWith) {
    HostTable plain;
    EXPECT_EQ(plain.map(1 * GB, 2 * GB, GB / PAGE_SIZE, RWX, ERROR_UNDO, nullptr), Status::Ok);
    EXPECT_EQ(leaf_size(plain, 1 * GB + 5 * MB), 1 * GB);

    HostTable splits(true);
    EXPECT_EQ(splits.map(1 * GB, 2 * GB, GB / PAGE_SIZE, RWX, ERROR_UNDO, nullptr), Status::Ok);
    EXPECT_EQ(leaf_size(splits, 1 * GB + 5 * MB), 2 * MB);
    EXPECT_EQ(splits.deposits().held(), 512U);
}

TEST(Map, ErrorIfExistingKeepSaysHowFarItGot) {
    HostTable table;
    table.leaf(0x40'2000, PageTableLevel::PageTable, 0x99'0000, USER_RWX);
    usize mapped = 0;
    EXPECT_EQ(table.map(0x40'0000, 0x10'0000, 4, USER_RWX, ERROR_KEEP, &mapped),
              Status::AlreadyExists);
    EXPECT_EQ(mapped, 2U);
    EXPECT_EQ(phys(table, 0x40'1000), 0x10'1000U);
    EXPECT_EQ(phys(table, 0x40'2000), 0x99'0000U);
    EXPECT_TRUE(table.query(0x40'3000).is_none());
}

TEST(Map, ErrorIfExistingUndoLeavesNothingOfTheCall) {
    HostTable table;
    table.leaf(0x40'2000, PageTableLevel::PageTable, 0x99'0000, USER_RWX);
    usize mapped = 7;
    EXPECT_EQ(table.map(0x40'0000, 0x10'0000, 4, USER_RWX, ERROR_UNDO, &mapped),
              Status::AlreadyExists);
    EXPECT_EQ(mapped, 0U);
    EXPECT_TRUE(table.query(0x40'0000).is_none());
    EXPECT_TRUE(table.query(0x40'1000).is_none());
    EXPECT_EQ(phys(table, 0x40'2000), 0x99'0000U);
}

TEST(Map, SkipIfExistingMapsAroundWhatIsThere) {
    HostTable table;
    table.leaf(0x40'2000, PageTableLevel::PageTable, 0x99'0000, USER_RWX);
    usize mapped = 0;
    EXPECT_EQ(table.map(0x40'0000, 0x10'0000, 4, USER_RWX, SKIP_UNDO, &mapped), Status::Ok);
    EXPECT_EQ(mapped, 4U);
    EXPECT_EQ(phys(table, 0x40'2000), 0x99'0000U);
    EXPECT_EQ(phys(table, 0x40'3000), 0x10'3000U);
}

TEST(Map, OverwriteIfExistingWritesOver) {
    HostTable table;
    table.leaf(0x40'2000, PageTableLevel::PageTable, 0x99'0000, MmuFlags::Readable);
    EXPECT_EQ(table.map(0x40'0000, 0x10'0000, 4, USER_RWX, OVERWRITE_UNDO, nullptr), Status::Ok);
    EXPECT_EQ(phys(table, 0x40'2000), 0x10'2000U);
    EXPECT_EQ(table.query(0x40'2000).value().flags, MmuFlags::Present | USER_RWX);
}

TEST(Map, OverwriteSplitsALargeLeafItCoversInPart) {
    HostTable table(true);
    table.leaf(0x60'0000, PageTableLevel::PageDirectory, 0x4000'0000, USER_RWX);
    EXPECT_EQ(table.map(0x60'1000, 0x10'0000, 1, RWX, OVERWRITE_UNDO, nullptr), Status::Ok);
    EXPECT_EQ(table.deposits().held(), 0U);
    EXPECT_EQ(phys(table, 0x60'1000), 0x10'0000U);
    EXPECT_EQ(phys(table, 0x60'2000), 0x4000'2000U);
    EXPECT_EQ(leaf_size(table, 0x60'2000), 4 * KB);
}

TEST(Map, NoMemoryPartWayIsUndoneOrKept) {
    // The first page needs three tables below the root; the second, past a
    // 2 MiB boundary, one more, which is not there.
    HostTable undo;
    undo.fail_allocations_after(3);
    usize mapped = 7;
    EXPECT_EQ(undo.map(0x1f'f000, 0x10'0000, 2, RWX, ERROR_UNDO, &mapped), Status::NoMemory);
    EXPECT_EQ(mapped, 0U);
    EXPECT_TRUE(undo.query(0x1f'f000).is_none());

    HostTable keep;
    keep.fail_allocations_after(3);
    EXPECT_EQ(keep.map(0x1f'f000, 0x10'0000, 2, RWX, ERROR_KEEP, &mapped), Status::NoMemory);
    EXPECT_EQ(mapped, 1U);
    EXPECT_EQ(phys(keep, 0x1f'f000), 0x10'0000U);
}

TEST(Map, AskingForWhatCannotBeIsInvArgs) {
    HostTable table;
    auto const both = arch::paging::MapOptions::ErrorIfExisting |
                      arch::paging::MapOptions::SkipIfExisting |
                      arch::paging::MapOptions::UndoOnFailure;
    EXPECT_EQ(table.map(0x40'0000, 0x10'0000, 1, RWX, both, nullptr), Status::InvArgs);
    EXPECT_EQ(table.map(0x40'0800, 0x10'0000, 1, RWX, ERROR_UNDO, nullptr), Status::InvArgs);
    EXPECT_EQ(table.map(0x40'0000, 0x10'0800, 1, RWX, ERROR_UNDO, nullptr), Status::InvArgs);
    EXPECT_EQ(table.map(0x40'0000, PhysAddr{1} << 52, 1, RWX, ERROR_UNDO, nullptr),
              Status::InvArgs);
    EXPECT_EQ(table.map(0x40'0000, 0x10'0000, 0, RWX, ERROR_UNDO, nullptr), Status::Ok);
}

TEST(MapFrames, MapsEachPageToItsOwnFrame) {
    HostTable table;
    auto const frames = std::array<PhysAddr, 4>{0xa0'0000, 0x50'0000, 0x30'0000, 0x70'0000};
    usize mapped = 0;
    EXPECT_EQ(table.map_frames(0x1f'e000, frames.data(), 4, USER_RWX, ERROR_UNDO, &mapped),
              Status::Ok);
    EXPECT_EQ(mapped, 4U);
    for (usize i = 0; i < 4; ++i) {
        EXPECT_EQ(phys(table, 0x1f'e000 + i * PAGE_SIZE), frames[i]) << i;
        EXPECT_EQ(leaf_size(table, 0x1f'e000 + i * PAGE_SIZE), 4 * KB) << i;
    }
}

TEST(MapFrames, SkipStepsOverALargeLeafAndItsFrames) {
    HostTable table;
    table.leaf(0x60'0000, PageTableLevel::PageDirectory, 0x4000'0000, RWX);
    std::vector<PhysAddr> frames(514);
    for (usize i = 0; i < frames.size(); ++i) {
        frames[i] = 0x1'0000'0000 + i * 2 * PAGE_SIZE;
    }
    EXPECT_EQ(table.map_frames(0x5f'f000, frames.data(), 514, RWX, SKIP_UNDO, nullptr), Status::Ok);
    EXPECT_EQ(phys(table, 0x5f'f000), frames[0]);
    EXPECT_EQ(phys(table, 0x60'0000), 0x4000'0000U);
    EXPECT_EQ(phys(table, 0x80'0000), frames[513]);
}

/// The same tables and the same maps, one run by the paging engine and one by
/// the walks here, end the same: what each call answered and said it mapped,
/// every page's entry, bit for bit, and the tables set aside.
TEST(Map, AgreesWithTheEngine) {
    for (bool const splits: {false, true}) {
        HostTable engine(splits);
        HostTable ours(splits);
        for (auto *table: {&engine, &ours}) {
            table->small_leaves(0x40'4000, 0x99'0000, 2, MmuFlags::Readable);
            table->leaf(0x80'0000, PageTableLevel::PageDirectory, 0x4020'0000, RWX);
        }
        std::vector<PhysAddr> frames(40);
        for (usize i = 0; i < frames.size(); ++i) {
            frames[i] = 0x7000'0000 + (frames.size() - i) * PAGE_SIZE;
        }

        struct Call {
            VirtAddr va;
            PhysAddr pa; // 0: the frames above
            usize n;
            arch::paging::MapOptions options;
        };
        auto const calls = std::array{
            Call{.va = 0x40'0000, .pa = 0x10'0000, .n = 8, .options = ERROR_KEEP},
            Call{.va = 0x40'0000, .pa = 0x10'0000, .n = 8, .options = SKIP_UNDO},
            Call{.va = 0x40'2000, .pa = 0x20'0000, .n = 4, .options = OVERWRITE_UNDO},
            Call{.va = 0x3f'f000, .pa = 0x5ff'f000, .n = 0x402, .options = ERROR_UNDO},
            Call{.va = 0x7f'e000, .pa = 0, .n = 40, .options = SKIP_UNDO},
            Call{.va = 0x80'4000, .pa = 0x30'0000, .n = 2, .options = OVERWRITE_UNDO},
            Call{.va = 0x1'0000'0000, .pa = 0x1'0000'0000, .n = 0x40001, .options = ERROR_UNDO},
            Call{.va = 0x7f'e000, .pa = 0, .n = 40, .options = ERROR_UNDO},
        };
        auto const allocator = engine.allocator();
        for (auto const &call: calls) {
            usize engine_mapped = 0;
            auto const expected = [&] {
                auto const flags = USER_RWX;
                if (call.pa == 0 && splits) {
                    return status_of(Engine::map_frames(
                        engine.root_phys(), identity, allocator, engine.deposits(), call.va,
                        frames.data(), call.n, flags, call.options, &engine_mapped));
                }
                if (call.pa == 0) {
                    return status_of(Engine::map_frames(
                        engine.root_phys(), identity, allocator, arch::paging::NoDeposits{},
                        call.va, frames.data(), call.n, flags, call.options, &engine_mapped));
                }
                if (splits) {
                    return status_of(Engine::map(engine.root_phys(), identity, allocator,
                                                 engine.deposits(), call.va, call.pa, call.n, flags,
                                                 call.options, &engine_mapped));
                }
                return status_of(Engine::map(engine.root_phys(), identity, allocator, call.va,
                                             call.pa, call.n, flags, call.options, &engine_mapped));
            }();
            usize ours_mapped = 0;
            auto const actual = [&] {
                if (call.pa == 0) {
                    return ours.map_frames(call.va, frames.data(), call.n, USER_RWX, call.options,
                                           &ours_mapped);
                }
                return ours.map(call.va, call.pa, call.n, USER_RWX, call.options, &ours_mapped);
            }();
            EXPECT_EQ(expected, actual) << std::hex << call.va << " splits " << splits;
            EXPECT_EQ(engine_mapped, ours_mapped) << std::hex << call.va << " splits " << splits;
        }

        for (VirtAddr va = 0x3f'0000; va < 0xc0'0000; va += PAGE_SIZE) {
            ASSERT_EQ(engine.leaf_entry(va), ours.leaf_entry(va))
                << std::hex << va << " splits " << splits;
        }
        for (VirtAddr va = 0x1'0000'0000; va < 0x1'4000'2000; va += 2 * MB) {
            ASSERT_EQ(engine.leaf_entry(va), ours.leaf_entry(va))
                << std::hex << va << " splits " << splits;
        }
        EXPECT_EQ(engine.deposits().held(), ours.deposits().held()) << "splits " << splits;
    }
}

TEST(SecondLevelEmpty, IsWhetherA2MibLeafCouldGoThere) {
    HostTable table(true);
    ASSERT_EQ(table.map(0x40'0000, 0x10'0000, 1, RWX, ERROR_UNDO, nullptr), Status::Ok);
    ASSERT_EQ(table.map(0x60'0000, 0x4000'0000, 512, RWX, ERROR_UNDO, nullptr), Status::Ok);
    table.leaf(1 * GB, PageTableLevel::PageDirectoryPointerTable, 2 * GB, RWX);

    EXPECT_FALSE(table.second_level_empty(0x41'0000)); // a table there
    EXPECT_FALSE(table.second_level_empty(0x60'0000)); // a 2 MiB leaf
    EXPECT_TRUE(table.second_level_empty(0x80'0000));
    EXPECT_FALSE(table.second_level_empty(1 * GB + 4 * MB)); // under a 1 GiB leaf
    EXPECT_TRUE(table.second_level_empty(0x80'0000'0000));   // no table at all

    // A table emptied by an unmap is still a table.
    ASSERT_EQ(table.unmap(0x40'0000, 1), Status::Ok);
    EXPECT_FALSE(table.second_level_empty(0x40'0000));
}

TEST(SecondLevelEmpty, AgreesWithTheEngine) {
    HostTable table(true);
    ASSERT_EQ(table.map(0x40'0000, 0x10'0000, 1, RWX, ERROR_UNDO, nullptr), Status::Ok);
    ASSERT_EQ(table.map(0x60'0000, 0x4000'0000, 512, RWX, ERROR_UNDO, nullptr), Status::Ok);
    table.leaf(1 * GB, PageTableLevel::PageDirectoryPointerTable, 2 * GB, RWX);
    for (VirtAddr va = 0; va < 2 * GB; va += 2 * MB) {
        EXPECT_EQ(Engine::second_level_empty(table.root_phys(), identity, va),
                  table.second_level_empty(va))
            << std::hex << va;
    }
}

/// Maps, into |table|, what the teardown tests free: 4 KiB leaves under three
/// tables of the last level, one 2 MiB leaf, and one leaf past 2 GiB.
auto fill_for_teardown(HostTable &table) -> void {
    ASSERT_EQ(table.map(0x40'0000, 0x10'0000, 2, RWX, ERROR_UNDO, nullptr), Status::Ok);
    ASSERT_EQ(table.map(0x60'0000, 0x4000'0000, 512, RWX, ERROR_UNDO, nullptr), Status::Ok);
    ASSERT_EQ(table.map(0x80'1000, 0x20'0000, 1, RWX, ERROR_UNDO, nullptr), Status::Ok);
    ASSERT_EQ(table.map(2 * GB, 0x30'0000, 1, RWX, ERROR_UNDO, nullptr), Status::Ok);
}

TEST(FreeTables, GivesEveryTableBackAndCountsTheLeaves) {
    HostTable table(true);
    fill_for_teardown(table);
    ASSERT_EQ(table.deposits().held(), 1U);

    auto const teardown = table.free_tables(0, 0);
    // Under the root's first entry: one table of 1 GiB entries, two of 2 MiB
    // entries (below 1 GiB and at 2 GiB), and three of 4 KiB entries.
    EXPECT_EQ(teardown.tables, 6U);
    EXPECT_EQ(teardown.leaves, 2U + 1U + 1U + 1U);
    EXPECT_EQ(table.freed(), 6U);
    EXPECT_EQ(table.deposits().held(), 0U);
    EXPECT_EQ(table.root_entry(0), 0U);
}

TEST(FreeTables, AgreesWithTheEngine) {
    for (bool const splits: {false, true}) {
        HostTable engine(splits);
        HostTable ours(splits);
        fill_for_teardown(engine);
        fill_for_teardown(ours);

        auto const expected = [&] {
            if (splits) {
                return Engine::free_tables(engine.root_phys(), identity, engine.deallocator(),
                                           engine.deposits(), 0, 0);
            }
            return Engine::free_tables(engine.root_phys(), identity, engine.deallocator(), 0, 0);
        }();
        auto const actual = ours.free_tables(0, 0);
        EXPECT_EQ(expected.tables, actual.tables) << "splits " << splits;
        EXPECT_EQ(expected.leaves, actual.leaves) << "splits " << splits;
        EXPECT_EQ(engine.freed(), ours.freed()) << "splits " << splits;
        EXPECT_EQ(engine.deposits().held(), ours.deposits().held()) << "splits " << splits;
        EXPECT_EQ(engine.root_entry(0), ours.root_entry(0)) << "splits " << splits;
    }
}

} // namespace
