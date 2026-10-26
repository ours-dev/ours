/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <page_tables/page_tables.hpp>

#include <array>
#include <deque>

#include <arch/paging.hpp>
#include <arch/paging/arch_mmu_flags.hpp>
#include <gtest/gtest.h>

namespace {
using arch::paging::MmuFlags;
using arch::paging::MmuFlagsConv;
using arch::paging::X86MmuFlags;
using page_tables::Mapping;
using page_tables::PageTableLevel;
using page_tables::PhysAddr;
using page_tables::PteVal;
using page_tables::VirtAddr;

constexpr usize KB = 1024;
constexpr usize MB = 1024 * KB;
constexpr usize GB = 1024 * MB;

/// A table as the hardware sees it: 512 entries, page aligned.
struct alignas(4 * KB) Table {
    std::array<PteVal, X86_MAX_PTES> entries{};
};

/// A page table in host memory, whose physical addresses are the host's own:
/// a table's address is where it is. The walks' contract, filled in the
/// simplest way it can be.
class HostTable final: public page_tables::X86PageTableImpl<HostTable> {
    friend class page_tables::X86PageTableImpl<HostTable>;

  public:
    HostTable() : m_root(make_table()) {}

    using X86PageTableImpl::query;

    /// The entry at |level| that |va| falls under, with a table made on the
    /// way down for every level above it that has none.
    auto entry(VirtAddr const va, PageTableLevel const level) -> PteVal & {
        auto at = top_level();
        auto table = m_root;
        while (at != level) {
            auto &slot = this->table(table)[page_tables::index(at, va)];
            if (!page_tables::is_present(slot)) {
                slot = make_table() | X86_MMUF_PRESENT | X86_MMUF_WRITABLE | X86_MMUF_USER;
            }
            table = page_tables::address(slot);
            at = page_tables::lower(at);
        }
        return this->table(table)[page_tables::index(level, va)];
    }

    /// Writes a leaf at |level| mapping |va| to |pa| with |flags|, as a map
    /// would have.
    auto leaf(VirtAddr const va, PageTableLevel const level, PhysAddr const pa,
              X86MmuFlags const flags) -> void {
        auto value = pa | PteVal(flags) | X86_MMUF_PRESENT;
        if (level != PageTableLevel::PageTable) {
            value |= X86_MMUF_PAGE_SIZE;
        }
        entry(va, level) = value;
    }

    [[nodiscard]]
    auto root_phys() const -> PhysAddr {
        return m_root;
    }

  private:
    static constexpr auto top_level() -> PageTableLevel {
        return PageTableLevel::PageMapLevel4;
    }

    [[nodiscard]]
    auto root() const -> PhysAddr {
        return m_root;
    }

    [[nodiscard]]
    auto table(PhysAddr const pa) const -> PteVal * {
        return reinterpret_cast<PteVal *>(pa);
    }

    [[nodiscard]]
    static auto to_mmu_flags(PteVal const entry, PageTableLevel) -> MmuFlags {
        return MmuFlagsConv<X86MmuFlags>::from_arch(X86MmuFlags(entry & X86_MMUF_MASK));
    }

    auto make_table() -> PhysAddr {
        return reinterpret_cast<PhysAddr>(m_tables.emplace_back().entries.data());
    }

    std::deque<Table> m_tables;
    PhysAddr m_root;
};

constexpr auto USER_RW = X86MmuFlags::User | X86MmuFlags::Writable;

TEST(Query, NothingMappedIsNone) {
    HostTable table;
    EXPECT_TRUE(table.query(0x40'0000).is_none());
}

TEST(Query, AFourKibLeafTranslatesTheOffsetWithinIt) {
    HostTable table;
    table.leaf(0x40'1000, PageTableLevel::PageTable, 0x1234'5000, USER_RW);

    auto const found = table.query(0x40'1abc);
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(found.value().phys, 0x1234'5abcU);
    EXPECT_EQ(found.value().size, 4 * KB);
    EXPECT_EQ(found.value().flags, MmuFlags::Present | MmuFlags::Readable | MmuFlags::Writable |
                                       MmuFlags::User | MmuFlags::Executable);
}

TEST(Query, ANeighbourInTheSameTableIsNone) {
    HostTable table;
    table.leaf(0x40'1000, PageTableLevel::PageTable, 0x1234'5000, USER_RW);
    EXPECT_TRUE(table.query(0x40'2000).is_none());
}

TEST(Query, LargeLeavesTranslateTheOffsetWithinThem) {
    HostTable table;
    table.leaf(0x20'0000, PageTableLevel::PageDirectory, 0x8000'0000, X86MmuFlags::NoExecutable);
    table.leaf(0x80'0000'0000, PageTableLevel::PageDirectoryPointerTable, 0x1'0000'0000,
               X86MmuFlags::Writable);

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
    using Engine =
        arch::paging::Paging<arch::paging::PagingDispatcher<PageTableLevel::PageMapLevel4>::Type>;

    HostTable table;
    table.leaf(0x40'0000, PageTableLevel::PageTable, 0x10'0000, USER_RW);
    table.leaf(0x40'1000, PageTableLevel::PageTable, 0x30'0000, X86MmuFlags::User);
    table.leaf(0x60'0000, PageTableLevel::PageDirectory, 0x4000'0000, X86MmuFlags::Writable);
    table.leaf(0xc0'0000'0000, PageTableLevel::PageDirectoryPointerTable, 0x8000'0000, {});

    auto const identity = [](PhysAddr const pa) -> VirtAddr { return pa; };
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

} // namespace
