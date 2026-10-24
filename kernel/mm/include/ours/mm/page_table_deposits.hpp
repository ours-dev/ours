/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PAGE_TABLE_DEPOSITS_HPP
#define OURS_MM_PAGE_TABLE_DEPOSITS_HPP 1

#include <ours/mm/types.hpp>

#include <arch/paging/deposits.hpp>

namespace ours::mm {
/// The tables set aside for the 2 MiB leaves of a user address space's page
/// tables, kept in the descriptors of the second-level tables that hold the
/// leaves (page::MmuBody): what the paging engine asks for when it maps,
/// splits and frees such leaves (arch::paging::TableDeposits).
///
/// No state of its own: everything is in the descriptors, so an architecture's
/// page table makes one wherever it calls the engine. Every call is made under
/// that page table's lock, which is what the descriptors' chains rely on; the
/// tables come from and go back to alloc_page_table() and free_page_table(),
/// which the engine's map already calls under it.
class PageTableDeposits {
  public:
    /// Sets a zeroed table aside with |owner|. False when the allocator has
    /// none to give.
    auto reserve(PhysAddr owner) -> bool;

    /// Takes one of |owner|'s tables back, to split a leaf into. The engine
    /// asks only for a leaf |owner| holds, and every leaf has one: an owner
    /// with none is a broken chain, and stops the kernel.
    auto withdraw(PhysAddr owner) -> PhysAddr;

    /// Frees one of |owner|'s tables, for a whole leaf unmapped.
    auto discard(PhysAddr owner) -> void;

    /// Frees all of |owner|'s tables, for |owner| about to be freed itself.
    auto discard_all(PhysAddr owner) -> void;

    /// How many tables are set aside in the whole system now: every one
    /// reserved and neither withdrawn nor discarded. For tests and diagnostics,
    /// which compare it before and after.
    [[nodiscard]]
    static auto nr_held() -> usize;
};
static_assert(::arch::paging::TableDeposits<PageTableDeposits>);

} // namespace ours::mm

#endif // #ifndef OURS_MM_PAGE_TABLE_DEPOSITS_HPP
