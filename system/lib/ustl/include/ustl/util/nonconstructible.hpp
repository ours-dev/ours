/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_UTIL_NONCONSTRUCTIBLE_HPP
#define USTL_UTIL_NONCONSTRUCTIBLE_HPP 1

namespace ustl {

struct NonConstructible {
    NonConstructible() = delete;
    ~NonConstructible() = delete;
};

}  // namespace ustl

#endif  // USTL_UTIL_NONCONSTRUCTIBLE_HPP
