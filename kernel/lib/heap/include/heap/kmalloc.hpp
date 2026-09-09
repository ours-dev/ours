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

auto init_kmalloc() -> void;

auto kmalloc(usize size, Gaf gaf) -> void *;

auto kmalloc(usize size, Gaf gaf, NnId nid) -> void *;

auto kfree(void *object) -> void;

} // namespace heap

#endif // #ifndef HEAP_KMALLOC_HPP