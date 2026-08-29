/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_COMPARABLE_HPP
#define GKTL_COMPARABLE_HPP 1

namespace gktl::mixin {
template <typename Derived>
class Comparable {
    friend auto operator==(Derived const &x, Derived const &y) -> bool {
        return &x == &y;
    }

    friend auto operator!=(Derived const &x, Derived const &y) -> bool {
        return &x != &y;
    }
};
} // namespace gktl::mixin

#endif // #ifndef GKTL_COMPARABLE_HPP