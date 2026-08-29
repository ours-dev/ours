/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_RWONCE_HPP
#define KTL_RWONCE_HPP 1

#include <ktl/types.hpp>

namespace ktl {
template <typename T, typename U>
FORCE_INLINE auto write_once(T &t, U const &val) -> void {
    *static_cast<T volatile *>(&t) = val;
}

template <typename T>
FORCE_INLINE auto read_once(T &t) -> T {
    return *static_cast<T volatile *>(&t);
}
} // namespace ktl

#endif // #ifndef KTL_RWONCE_HPP