/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_NEW_HPP
#define KTL_NEW_HPP 1

#include <ktl/kmalloc.hpp>

auto operator new(usize size, ktl::Gaf gaf) -> void *;

auto operator new[](usize size, ktl::Gaf gaf) -> void *;

auto operator new(usize size, ktl::Gaf gaf, ktl::NnId nid) -> void *;

auto operator new[](usize size, ktl::Gaf gaf, ktl::NnId nid) -> void *;

// The deallocation functions carry noexcept because the standard gives them
// that specification; libc++'s <new> insists on it when a kernel header lands
// in a hosted build (the unit tests).
auto operator delete(void *ptr) noexcept -> void;

auto operator delete[](void *ptr) noexcept -> void;

#endif // #ifndef KTL_NEW_HPP