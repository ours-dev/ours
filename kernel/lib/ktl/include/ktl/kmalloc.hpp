/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_KMALLOC_HPP
#define KTL_KMALLOC_HPP 1

#include <ktl/types.hpp>

namespace ktl {
auto init_kmalloc() -> void;

auto kmalloc(usize size, Gaf gaf) -> void *;

auto kmalloc(usize size, Gaf gaf, NnId nid) -> void *;

auto kfree(void *object) -> void;

} // namespace ktl

#endif // #ifndef KTL_KMALLOC_HPP