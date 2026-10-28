/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The paging engine over Sv39, on the host: page tables in host memory under
/// made-up physical addresses, filled by map(), read back by query(), cleared
/// by unmap(), and every entry those calls write compared bit for bit with a
/// value worked out by hand from the RISC-V privileged specification.
///
/// The working uses the specification's layout of an Sv39 entry:
///
///   63..54  reserved (and extension bits this layer does not use): 0
///   53..10  PPN, the physical address >> 12
///    9..8   RSW, left to software: 0
///    7 D  6 A  5 G  4 U  3 X  2 W  1 R  0 V
///
/// so an entry's value is (pa >> 12) << 10, which is pa >> 2 for an aligned
/// pa, ORed with its flag bits. A table entry is V alone with the PPN; a leaf
/// has R, W or X.

#include <arch/paging.hpp>
#include <arch/paging/riscv_pagings.hpp>

#include <iterator>

#include <cstdio>
#include <cstring>

namespace {
using namespace arch::paging;

using Sv39 = PagingDispatcher<RiscvPagingLevel::Level2>::Type;
using PageMap = arch::Paging<Sv39>;
using Traits = PageMap::Traits;

/// Page tables in host memory, each known to the engine by a made-up physical
/// address so that an entry pointing at one has a value fixed in advance:
/// table i is at BASE + i * 4 KiB, and table 0 is the root. An address
/// outside them -- what a walk reaches if it takes a leaf for a table -- is
/// given a scratch page and counted, so a broken encoding fails a check
/// instead of writing through a wild pointer.
struct Tables {
    static constexpr ours::PhysAddr BASE = 0x8010'0000;
    static constexpr ours::PhysAddr ROOT = BASE;
    static constexpr usize COUNT = 4;

    alignas(PAGE_SIZE) unsigned char pages[COUNT][PAGE_SIZE] = {};
    alignas(PAGE_SIZE) unsigned char stray[PAGE_SIZE] = {};
    usize next = 1;
    usize strays = 0;

    auto allocate(usize const size, usize const alignment) -> ours::PhysAddr {
        if (size != PAGE_SIZE || alignment != PAGE_SIZE || next == COUNT) {
            return 0;
        }
        auto const table = BASE + next * PAGE_SIZE;
        next += 1;
        return table;
    }

    auto to_host(ours::PhysAddr const pa) -> ours::VirtAddr {
        if (pa < BASE || pa >= BASE + COUNT * PAGE_SIZE || pa % PAGE_SIZE != 0) {
            strays += 1;
            return reinterpret_cast<ours::VirtAddr>(stray);
        }
        return reinterpret_cast<ours::VirtAddr>(pages[(pa - BASE) / PAGE_SIZE]);
    }

    /// Entry |index| of table |table|, as the hardware would read it.
    [[nodiscard]]
    auto entry(usize const table, usize const index) const -> u64 {
        u64 value = 0;
        std::memcpy(&value, pages[table] + index * sizeof(u64), sizeof(u64));
        return value;
    }

    /// How many entries of table |table| are not zero.
    [[nodiscard]]
    auto nr_written(usize const table) const -> usize {
        usize count = 0;
        for (usize i = 0; i < PAGE_SIZE / sizeof(u64); ++i) {
            if (entry(table, i) != 0) {
                count += 1;
            }
        }
        return count;
    }

    /// What the engine calls to reach a table through its physical address.
    struct ToHost {
        Tables *tables;

        auto operator()(ours::PhysAddr const pa) const -> ours::VirtAddr {
            return tables->to_host(pa);
        }
    };

    /// map() into this root, with a table from this pool when one is needed.
    auto map(ours::VirtAddr const va, ours::PhysAddr const pa, usize const pages_to_map,
             MmuFlags const flags) -> ustl::Result<void, MapError> {
        auto allocator = [this](usize const size, usize const alignment) {
            return allocate(size, alignment);
        };
        return PageMap::map(ROOT, ToHost{this}, allocator, va, pa, pages_to_map, flags,
                            MapOptions::ErrorIfExisting | MapOptions::UndoOnFailure);
    }

    auto query(ours::VirtAddr const va) -> ustl::Option<QueryResult> {
        return PageMap::query(ROOT, ToHost{this}, va);
    }

    auto unmap(ours::VirtAddr const va, usize const pages_to_unmap)
        -> ustl::Result<usize, MapError> {
        return PageMap::unmap(ROOT, ToHost{this}, va, pages_to_unmap);
    }

    auto protect(ours::VirtAddr const va, usize const pages_to_protect, MmuFlags const flags)
        -> ustl::Result<usize, MapError> {
        return PageMap::protect(ROOT, ToHost{this}, va, pages_to_protect, flags);
    }
};

/// Says what differs, so a failing run names the bits.
auto expect(char const *what, u64 const actual, u64 const expected) -> bool {
    if (actual == expected) {
        return true;
    }
    std::printf("  %s: got %#018llx, expected %#018llx\n", what,
                static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
    return false;
}

auto expect_query(char const *what, ustl::Option<QueryResult> const &result,
                  ours::PhysAddr const phys, usize const size, MmuFlags const flags) -> bool {
    if (!result) {
        std::printf("  %s: nothing mapped\n", what);
        return false;
    }
    auto ok = expect(what, result.value().phys, phys);
    ok = expect(what, result.value().size, size) && ok;
    return expect(what, static_cast<u64>(result.value().flags), static_cast<u64>(flags)) && ok;
}

auto expect_absent(char const *what, ustl::Option<QueryResult> const &result) -> bool {
    if (!result) {
        return true;
    }
    std::printf("  %s: still maps %#llx\n", what,
                static_cast<unsigned long long>(result.value().phys));
    return false;
}

constexpr auto KERNEL_RW = MmuFlags::Present | MmuFlags::Readable | MmuFlags::Writable;
constexpr auto KERNEL_RWX = KERNEL_RW | MmuFlags::Executable;
constexpr auto USER_RX =
    MmuFlags::Present | MmuFlags::Readable | MmuFlags::Executable | MmuFlags::User;

/// The three mappings, one per leaf size, chosen so that each level's index
/// differs from the others' -- a walk that read one level's index bits for
/// another's would land on a different entry -- and so that the 4 KiB and
/// 2 MiB ones share the root entry and the level-1 table below it.
///
/// VA_4K = 0x1'40e0'9000 = 5 << 30 | 7 << 21 | 9 << 12: VPN[2] = 5, VPN[1] = 7,
/// VPN[0] = 9. VA_2M = 0x1'4100'0000 = 5 << 30 | 8 << 21: VPN[2] = 5,
/// VPN[1] = 8. VA_1G = 0x1'8000'0000 = 6 << 30: VPN[2] = 6. Bit 38 is clear in
/// all three, so all are in Sv39's lower half.
constexpr ours::VirtAddr VA_4K = 0x1'40e0'9000;
constexpr ours::VirtAddr VA_2M = 0x1'4100'0000;
constexpr ours::VirtAddr VA_1G = 0x1'8000'0000;

/// A megapage's frame must be 2 MiB aligned (PPN[0] = 0) and a gigapage's
/// 1 GiB aligned (PPN[1] = PPN[0] = 0), or the hardware faults on them.
constexpr ours::PhysAddr PA_4K = 0x8765'4000;
constexpr ours::PhysAddr PA_2M = 0x8040'0000;
constexpr ours::PhysAddr PA_1G = 0xc000'0000;

/// Root entry 5 -> table 1 at 0x8010'1000: PPN 0x80101, << 10 = 0x2004'0400,
/// and V (bit 0) alone: R = W = X = 0 makes it a pointer, and U, A, D, G are
/// clear.
constexpr u64 ROOT_TO_TABLE_1 = 0x2004'0401;

/// Table 1 entry 7 -> table 2 at 0x8010'2000: PPN 0x80102, << 10 =
/// 0x2004'0800, | V.
constexpr u64 TABLE_1_TO_TABLE_2 = 0x2004'0801;

/// The 4 KiB leaf, kernel read-write: PPN 0x87654, << 10 = 0x21d9'5000.
/// V 0x01 | R 0x02 | W 0x04 | G 0x20 (no U: a kernel page) | A 0x40 |
/// D 0x80 (writable) = 0xe7.
constexpr u64 LEAF_4K = 0x21d9'50e7;

/// The 2 MiB leaf, user read-execute: PPN 0x80400, << 10 = 0x2010'0000.
/// V 0x01 | R 0x02 | X 0x08 | U 0x10 | A 0x40 = 0x5b: no G (a user page), no D
/// (not writable).
constexpr u64 LEAF_2M = 0x2010'005b;

/// The 1 GiB leaf, kernel read-write-execute: PPN 0xc0000, << 10 =
/// 0x3000'0000. V | R | W | X 0x08 | G | A | D = 0xef.
constexpr u64 LEAF_1G = 0x3000'00ef;

/// Sv39's shape, as the engine reads it through PagingTraits.
auto check_sv39_shape() -> bool {
    auto ok = expect("top level", usize(Traits::PAGING_LEVEL), 2);
    ok = expect("final level", usize(Traits::FINAL_LEVEL), 0) && ok;
    ok = expect("second level", usize(Traits::SECOND_LEVEL), 1) && ok;
    ok = expect("satp mode", static_cast<u64>(Sv39::SATP_MODE), 8) && ok;
    ok = expect("physical address bits", Traits::MAX_PHYS_ADDR_SIZE, 56) && ok;
    ok = expect("entries per table", Traits::NUM_PTES<RiscvPagingLevel::Level2>, 512) && ok;
    ok = expect("table size", Traits::SIZE_OF_TABLE<RiscvPagingLevel::Level0>, 4_kb) && ok;
    ok = expect("level 0 page", Traits::page_size(RiscvPagingLevel::Level0), 4_kb) && ok;
    ok = expect("level 1 page", Traits::page_size(RiscvPagingLevel::Level1), 2_mb) && ok;
    ok = expect("level 2 page", Traits::page_size(RiscvPagingLevel::Level2), 1_gb) && ok;
    ok = expect("VPN[2]", Traits::virt_to_index(RiscvPagingLevel::Level2, VA_4K), 5) && ok;
    ok = expect("VPN[1]", Traits::virt_to_index(RiscvPagingLevel::Level1, VA_4K), 7) && ok;
    ok = expect("VPN[0]", Traits::virt_to_index(RiscvPagingLevel::Level0, VA_4K), 9) && ok;
    // An upper-half address, 0xffff'ffc0'0000'0000 + 3 GiB: bits 63..38 set, so VPN[2] is
    // bits 38..30 = 0b1'0000'0011 = 0x103.
    ok = expect("upper VPN[2]",
                Traits::virt_to_index(RiscvPagingLevel::Level2, 0xffff'ffc0'c000'0000), 0x103) &&
         ok;
    return ok && Traits::VIRT_ADDR_EXT == VirtAddrExt::Canonical;
}

/// Maps the three leaves into one root and checks every entry written: two
/// tables allocated, the two pointers and three leaves at the entries worked
/// out above, and nothing else anywhere.
auto map_three_leaves(Tables &tables) -> bool {
    if (tables.map(VA_4K, PA_4K, 1, KERNEL_RW).is_err() ||
        tables.map(VA_2M, PA_2M, 2_mb / PAGE_SIZE, USER_RX).is_err() ||
        tables.map(VA_1G, PA_1G, 1_gb / PAGE_SIZE, KERNEL_RWX).is_err()) {
        std::printf("  map failed\n");
        return false;
    }
    return true;
}

auto check_encodings() -> bool {
    Tables tables;
    if (!map_three_leaves(tables)) {
        return false;
    }
    auto ok = expect("tables allocated", tables.next - 1, 2);
    ok = expect("root[5], a table entry", tables.entry(0, 5), ROOT_TO_TABLE_1) && ok;
    ok = expect("root[6], the 1 GiB leaf", tables.entry(0, 6), LEAF_1G) && ok;
    ok = expect("table 1[7], a table entry", tables.entry(1, 7), TABLE_1_TO_TABLE_2) && ok;
    ok = expect("table 1[8], the 2 MiB leaf", tables.entry(1, 8), LEAF_2M) && ok;
    ok = expect("table 2[9], the 4 KiB leaf", tables.entry(2, 9), LEAF_4K) && ok;
    ok = expect("root entries written", tables.nr_written(0), 2) && ok;
    ok = expect("table 1 entries written", tables.nr_written(1), 2) && ok;
    ok = expect("table 2 entries written", tables.nr_written(2), 1) && ok;
    return expect("stray table accesses", tables.strays, 0) && ok;
}

/// query() at an offset inside each leaf: the frame plus the offset, the
/// leaf's size, and the flags it was mapped with.
auto check_queries() -> bool {
    Tables tables;
    if (!map_three_leaves(tables)) {
        return false;
    }
    auto ok = expect_query("4 KiB", tables.query(VA_4K + 0x123), PA_4K + 0x123, 4_kb, KERNEL_RW);
    ok = expect_query("2 MiB", tables.query(VA_2M + 7 * PAGE_SIZE + 0x10),
                      PA_2M + 7 * PAGE_SIZE + 0x10, 2_mb, USER_RX) &&
         ok;
    ok = expect_query("1 GiB", tables.query(VA_1G + 0x1234'5678), PA_1G + 0x1234'5678, 1_gb,
                      KERNEL_RWX) &&
         ok;
    ok = expect_absent("the page after the 4 KiB leaf", tables.query(VA_4K + PAGE_SIZE)) && ok;
    return expect("stray table accesses", tables.strays, 0) && ok;
}

/// unmap() of each leaf clears exactly that entry; the table entries above
/// stay, since unmapping never frees a table.
auto check_unmaps() -> bool {
    Tables tables;
    if (!map_three_leaves(tables)) {
        return false;
    }
    auto const four_k = tables.unmap(VA_4K, 1);
    auto const two_m = tables.unmap(VA_2M, 2_mb / PAGE_SIZE);
    auto const one_g = tables.unmap(VA_1G, 1_gb / PAGE_SIZE);
    if (four_k.is_err() || two_m.is_err() || one_g.is_err()) {
        std::printf("  unmap failed\n");
        return false;
    }
    auto ok = expect("table 2[9] after unmap", tables.entry(2, 9), 0);
    ok = expect("table 1[8] after unmap", tables.entry(1, 8), 0) && ok;
    ok = expect("root[6] after unmap", tables.entry(0, 6), 0) && ok;
    ok = expect("root[5] kept", tables.entry(0, 5), ROOT_TO_TABLE_1) && ok;
    ok = expect("table 1[7] kept", tables.entry(1, 7), TABLE_1_TO_TABLE_2) && ok;
    ok = expect_absent("4 KiB after unmap", tables.query(VA_4K)) && ok;
    ok = expect_absent("2 MiB after unmap", tables.query(VA_2M)) && ok;
    ok = expect_absent("1 GiB after unmap", tables.query(VA_1G)) && ok;
    return expect("stray table accesses", tables.strays, 0) && ok;
}

/// The two requests a leaf cannot encode get read access added, and
/// execute-only, which it can, stays as asked. Three pages from VA_4K, to
/// PA_4K and the two frames after it (PPN 0x87655 and 0x87656, << 10 =
/// 0x21d9'5400 and 0x21d9'5800):
///   - W alone: V | R | W | G | A | D = 0xe7, R added (W without R is
///     reserved, and faults);
///   - nothing: V | R | G | A = 0x63, R added (R = W = X = 0 is a pointer);
///   - X alone: V | X | G | A = 0x69, a valid execute-only leaf.
auto check_leaf_access() -> bool {
    Tables tables;
    auto const write_only = MmuFlags::Present | MmuFlags::Writable;
    auto const no_access = MmuFlags::Present;
    auto const execute_only = MmuFlags::Present | MmuFlags::Executable;
    if (tables.map(VA_4K, PA_4K, 1, write_only).is_err() ||
        tables.map(VA_4K + PAGE_SIZE, PA_4K + PAGE_SIZE, 1, no_access).is_err() ||
        tables.map(VA_4K + 2 * PAGE_SIZE, PA_4K + 2 * PAGE_SIZE, 1, execute_only).is_err()) {
        std::printf("  map failed\n");
        return false;
    }
    auto ok = expect("W alone", tables.entry(2, 9), 0x21d9'50e7);
    ok = expect("no permission", tables.entry(2, 10), 0x21d9'5463) && ok;
    ok = expect("X alone", tables.entry(2, 11), 0x21d9'5869) && ok;
    ok = expect_query("W alone", tables.query(VA_4K), PA_4K, 4_kb, KERNEL_RW) && ok;
    ok = expect_query("no permission", tables.query(VA_4K + PAGE_SIZE), PA_4K + PAGE_SIZE, 4_kb,
                      MmuFlags::Present | MmuFlags::Readable) &&
         ok;
    ok = expect_query("X alone", tables.query(VA_4K + 2 * PAGE_SIZE), PA_4K + 2 * PAGE_SIZE, 4_kb,
                      execute_only) &&
         ok;
    return expect("stray table accesses", tables.strays, 0) && ok;
}

/// protect() takes W away from the kernel read-write leaf: 0x21d9'50e7 loses
/// W 0x04 and keeps D, which only says the page may have been written, so
/// 0x21d9'50e3. Taking every permission away then leaves it there: a leaf with
/// none would be a pointer, so it stays readable.
auto check_protect() -> bool {
    Tables tables;
    if (tables.map(VA_4K, PA_4K, 1, KERNEL_RW).is_err()) {
        std::printf("  map failed\n");
        return false;
    }
    auto const read_only = tables.protect(VA_4K, 1, MmuFlags::Present | MmuFlags::Readable);
    auto ok = read_only.is_ok() && expect("read-only", tables.entry(2, 9), 0x21d9'50e3);
    ok = expect_query("read-only", tables.query(VA_4K), PA_4K, 4_kb,
                      MmuFlags::Present | MmuFlags::Readable) &&
         ok;
    auto const none = tables.protect(VA_4K, 1, MmuFlags::Present);
    ok = none.is_ok() && expect("no permission", tables.entry(2, 9), 0x21d9'50e3) && ok;
    return expect("stray table accesses", tables.strays, 0) && ok;
}

/// The PPN is 44 bits, so the last frame below 2^56, 0xff'ffff'ffff'f000,
/// is PPN 0xfff'ffff'ffff: bits 53..10 all set, 0x3f'ffff'ffff'fc00, | 0xe7.
/// The frame at 2^56 does not fit and is refused before anything is written.
auto check_physical_address_width() -> bool {
    Tables tables;
    constexpr ours::PhysAddr LAST_FRAME = 0x00ff'ffff'ffff'f000;
    if (tables.map(VA_4K, LAST_FRAME, 1, KERNEL_RW).is_err()) {
        std::printf("  map of the last frame failed\n");
        return false;
    }
    auto ok = expect("last frame", tables.entry(2, 9), 0x003f'ffff'ffff'fce7);
    ok = expect_query("last frame", tables.query(VA_4K), LAST_FRAME, 4_kb, KERNEL_RW) && ok;
    auto const beyond = tables.map(VA_4K + PAGE_SIZE, 0x0100'0000'0000'0000, 1, KERNEL_RW);
    ok = expect("frame at 2^56 refused", beyond.is_err() && beyond.error() == MapError::InvArgs,
                true) &&
         ok;
    return expect("entry after the refused one", tables.entry(2, 10), 0) && ok;
}

/// satp = MODE << 60 | ASID << 44 | root PPN. The root at 0x8010'0000 is PPN
/// 0x80100; Sv39 is MODE 8:
///   ASID 0:      0x8000'0000'0000'0000 | 0x8'0100 = 0x8000'0000'0008'0100;
///   ASID 0x1234: | 0x1234 << 44 = 0x0123'4000'0000'0000, so
///                0x8123'4000'0008'0100.
auto check_satp() -> bool {
    auto ok = expect("satp, ASID 0", Sv39::satp(Tables::ROOT, 0), 0x8000'0000'0008'0100);
    return expect("satp, ASID 0x1234", Sv39::satp(Tables::ROOT, 0x1234), 0x8123'4000'0008'0100) &&
           ok;
}

struct Check {
    char const *name;
    auto (*run)() -> bool;
};
} // namespace

auto main() -> int {
    constexpr Check CHECKS[] = {
        {"sv39 shape", check_sv39_shape},
        {"leaf and table encodings", check_encodings},
        {"queries", check_queries},
        {"unmaps", check_unmaps},
        {"leaf access", check_leaf_access},
        {"protect", check_protect},
        {"physical address width", check_physical_address_width},
        {"satp", check_satp},
    };
    usize failed = 0;
    for (auto const &check: CHECKS) {
        if (!check.run()) {
            std::printf("FAIL %s\n", check.name);
            failed += 1;
        }
    }
    if (failed != 0) {
        std::printf("riscv-paging-smoke: %zu of %zu checks failed\n", failed, std::size(CHECKS));
        return 1;
    }
    std::printf("riscv-paging-smoke: %zu checks passed\n", std::size(CHECKS));
    return 0;
}
