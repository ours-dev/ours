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

    using X86PageTableImpl::protect;
    using X86PageTableImpl::query;
    using X86PageTableImpl::unmap;

    /// The table at |level| that |va| falls under, with a table made on the
    /// way down for every level above it that has none.
    auto owner(VirtAddr const va, PageTableLevel const level) -> PhysAddr {
        auto at = top_level();
        auto pa = m_root;
        while (at != level) {
            auto &slot = table(pa)[page_tables::index(at, va)];
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
        table(in)[page_tables::index(level, va)] = MmuFormat::leaf(level, pa, flags);
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
            auto const entry = table(pa)[page_tables::index(level, va)];
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
    static auto table(PhysAddr const pa) -> PteVal * {
        return reinterpret_cast<PteVal *>(pa);
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

    Arena m_arena;
    HostDeposits m_deposits;
    PhysAddr m_root;
    bool m_splits;
};

using Engine =
    arch::paging::Paging<arch::paging::PagingDispatcher<PageTableLevel::PageMapLevel4>::Type>;

auto identity(PhysAddr const pa) -> VirtAddr {
    return pa;
}

/// What the kernel's PageTable made of the engine's result.
auto status_of(ustl::Result<usize, arch::paging::MapError> const &result) -> Status {
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

} // namespace
