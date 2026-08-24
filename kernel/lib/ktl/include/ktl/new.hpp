/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_NEW_HPP
#define KTL_NEW_HPP 1

#include <ktl/kmalloc.hpp>

auto operator new(ktl::usize size, ktl::Gaf gaf) -> void *;

auto operator new[](ktl::usize size, ktl::Gaf gaf) -> void *;

auto operator new(ktl::usize size, ktl::Gaf gaf, ktl::NnId nid) -> void *;

auto operator new[](ktl::usize size, ktl::Gaf gaf, ktl::NnId nid) -> void *;

auto operator delete(void *ptr) -> void;

auto operator delete[](void *ptr) -> void;

#endif // #ifndef KTL_NEW_HPP