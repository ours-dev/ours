/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef HEAP_KMALLOC_HPP
#define HEAP_KMALLOC_HPP 1

#include <ours/mm/gaf.hpp>
#include <ours/types.hpp>

namespace heap {
/// Spelled through ours::mm rather than through ktl's aliases for those same
/// types: ktl depends on //kernel/mm:headers, and this library depends on mm
/// too, so taking the alias would put ktl on the path between them for no
/// reason. The types are identical either way -- ktl::Gaf *is*
/// ours::mm::Gaf -- so nothing at a call site changes.
using ours::NnId;
using ours::mm::Gaf;

/// The size-class ladder, and with it the largest request kmalloc can serve.
///
/// These live in the header because the boundary is not private any more:
/// `operator new` routes above it to vmalloc, and a threshold restated in
/// new.cpp would be a second definition free to drift from this one. Cache |i|
/// serves objects of 2^(i + KMALLOC_MIN_ORDER) bytes, so deriving the bound
/// from the last cache rather than writing 4096 keeps an off-by-one here from
/// indexing past the end of the cache array.
inline constexpr auto KMALLOC_NUM_CACHES = usize{10};
inline constexpr auto KMALLOC_MIN_ORDER = usize{3};
inline constexpr auto KMALLOC_MIN_SIZE = usize{1} << KMALLOC_MIN_ORDER;
inline constexpr auto KMALLOC_MAX_SIZE = usize{1} << (KMALLOC_NUM_CACHES - 1 + KMALLOC_MIN_ORDER);

auto init_kmalloc() -> void;

auto kmalloc(usize size, Gaf gaf) -> void *;

auto kmalloc(usize size, Gaf gaf, NnId nid) -> void *;

auto kfree(void *object) -> void;

} // namespace heap

#endif // #ifndef HEAP_KMALLOC_HPP