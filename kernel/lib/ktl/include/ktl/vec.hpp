/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_VEC_HPP
#define KTL_VEC_HPP 1

#include <ktl/allocator.hpp>
#include <ustl/collections/vec.hpp>

namespace ktl {
    template <typename T, typename A = ktl::Allocator<T>>
    using Vec = ::ustl::collections::Vec<T, A>;

} // namespace ktl

#endif // #ifndef KTL_VEC_HPP