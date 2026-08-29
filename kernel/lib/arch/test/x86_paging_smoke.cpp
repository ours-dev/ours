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
    return unaligned.is_err() && unaligned.error() == MapError::InvalidArgument;
}

auto verify_unmapping_removes_an_existing_mapping() -> bool {
    alignas(PAGE_SIZE) unsigned char root[PAGE_SIZE] = {};
    PageTablePool pool;
    auto allocator = [&pool](usize size, usize alignment) {
        return pool.allocate(size, alignment);
    };
    auto const virtual_address = MB(4);
    auto const physical_address = MB(6);
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
    auto const virtual_address = MB(8);
    auto const physical_address = MB(10);
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
    auto const virtual_address = MB(16);
    auto const physical_address = MB(18);
    auto const flags = MmuFlags::Present | MmuFlags::Writable;

    auto const mapped =
        PageMap::map(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{}, allocator,
                     virtual_address, physical_address, MB(2) / PAGE_SIZE, flags);
    if (mapped.is_err()) {
        return false;
    }

    auto const offset = PAGE_SIZE * 7;
    auto const query = PageMap::query(reinterpret_cast<ours::PhysAddr>(root), IdentityPhysToVirt{},
                                      virtual_address + offset);
    return query && query.value().size == MB(2) && query.value().phys == physical_address + offset;
}
} // namespace

auto main() -> int {
    if (!verify_mapping(MB(2), MB(2), 1, PAGE_SIZE)) {
        return 1;
    }
    if (!verify_mapping(MB(2), MB(2), MB(2) / PAGE_SIZE, MB(2))) {
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
    return 0;
}
