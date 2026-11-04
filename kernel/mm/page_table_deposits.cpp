/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/page_table_deposits.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/mm/pmm.hpp>

#include <atomic>

namespace ours::mm {
namespace {
    /// Tables set aside and not yet withdrawn or discarded, system-wide.
    /// Relaxed: a count for tests and diagnostics, not a protocol.
    constinit std::atomic<usize> NR_HELD{0};

    auto body_of(PhysAddr const table) -> page::MmuBody & {
        return phys_to_frame(table)->as<page::MmuBody>();
    }

    /// Takes the first table off |owner|'s chain, or null when there is none.
    auto pop(PhysAddr const owner) -> PageFrame * {
        auto &body = body_of(owner);
        auto *const first = body.deposits;
        if (first == nullptr) {
            return nullptr;
        }
        auto &first_body = first->as<page::MmuBody>();
        body.deposits = first_body.next_deposit;
        first_body.next_deposit = nullptr;
        NR_HELD.fetch_sub(1, std::memory_order_relaxed);
        return first;
    }
} // namespace

auto PageTableDeposits::reserve(PhysAddr const owner) -> bool {
    auto const table = alloc_page_table(PAGE_SIZE, PAGE_SIZE);
    if (table == PhysAddr{0}) {
        return false;
    }
    auto &body = body_of(owner);
    auto *const frame = phys_to_frame(table);
    frame->as<page::MmuBody>().next_deposit = body.deposits;
    body.deposits = frame;
    NR_HELD.fetch_add(1, std::memory_order_relaxed);
    return true;
}

auto PageTableDeposits::withdraw(PhysAddr const owner) -> PhysAddr {
    auto *const frame = pop(owner);
    ASSERT(frame != nullptr, "the page table {:#x} has a large leaf but no table set aside for it",
           owner);
    return frame_to_phys(frame);
}

auto PageTableDeposits::discard(PhysAddr const owner) -> void {
    auto *const frame = pop(owner);
    ASSERT(frame != nullptr, "the page table {:#x} has a large leaf but no table set aside for it",
           owner);
    free_page_table(frame_to_phys(frame), PAGE_SIZE);
}

auto PageTableDeposits::discard_all(PhysAddr const owner) -> void {
    while (auto *const frame = pop(owner)) {
        free_page_table(frame_to_phys(frame), PAGE_SIZE);
    }
}

auto PageTableDeposits::nr_held() -> usize {
    return NR_HELD.load(std::memory_order_relaxed);
}

} // namespace ours::mm
