/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_DEPOSITS_HPP
#define ARCH_PAGING_DEPOSITS_HPP 1

#include <arch/types.hpp>

#include <concepts>
#include <type_traits>

namespace arch::paging {
/// Page tables set aside for large leaves, so that a leaf can be split into
/// final-level entries without allocating.
///
/// A caller that passes one to Paging's map(), unmap(), protect() and
/// free_tables() asks for the discipline of an address space whose mappings
/// change in part: a leaf above the final level is mapped only at the second
/// level (2 MiB on x86) and only with a table set aside for it, and a change
/// to part of such a leaf first splits it into that table, which allocates
/// nothing and cannot fail. Where the tables are kept is the caller's: the
/// engine names them only by |owner|, the table that holds the leaf's entry,
/// and any table set aside with an owner serves any leaf in it.
///
///  - reserve(owner): sets a zeroed table aside with |owner|, for a leaf about
///    to be written there. False when none can be had; no leaf is written
///    then.
///  - withdraw(owner): takes one of |owner|'s tables back, to split a leaf
///    into. There is one for every leaf |owner| holds.
///  - discard(owner): gives one of |owner|'s tables back to the allocator,
///    when a whole leaf goes.
///  - discard_all(owner): gives every one of |owner|'s back, when |owner|
///    itself is freed.
template <typename T>
concept TableDeposits = requires(T &deposits, PhysAddr const owner) {
    { deposits.reserve(owner) } -> std::same_as<bool>;
    { deposits.withdraw(owner) } -> std::same_as<PhysAddr>;
    { deposits.discard(owner) } -> std::same_as<void>;
    { deposits.discard_all(owner) } -> std::same_as<void>;
};

/// No deposits: the discipline of the kernel's own mappings, which are never
/// changed in part. A leaf may be at any level that can hold one, and a change
/// to part of a large leaf is InvArgs.
struct NoDeposits {};

/// What a deposits argument may be: a TableDeposits, or NoDeposits.
template <typename T>
concept DepositDiscipline =
    TableDeposits<std::remove_cvref_t<T>> || std::same_as<std::remove_cvref_t<T>, NoDeposits>;

} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_DEPOSITS_HPP
