/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_UTIL_NONCOPYABLE_HPP
#define USTL_UTIL_NONCOPYABLE_HPP 1

#define USTL_NOMOVEABLE(TYPE) \
    TYPE(TYPE &&) = delete; \
    auto operator=(TYPE &&) -> TYPE & = delete

#define USTL_NOCOPYABLE(TYPE) \
    TYPE(TYPE const &) = delete; \
    auto operator=(TYPE const &) -> TYPE & = delete

#define USTL_NO_MOVEABLE_AND_COPYABLE(TYPE) \
    USTL_NOMOVEABLE(TYPE); \
    USTL_NOCOPYABLE(TYPE)

#define USTL_NOT_COPYABLE_AND_MOVEABLE(TYPE) USTL_NO_MOVEABLE_AND_COPYABLE(TYPE)

namespace ustl {

struct NonMovable {
    NonMovable(NonMovable &&) = delete;
    auto operator=(NonMovable &&) -> NonMovable & = delete;
};

struct NonCopyable {
    NonCopyable(NonCopyable const &) = delete;
    auto operator=(NonCopyable const &) -> NonCopyable & = delete;
};

}  // namespace ustl

#endif  // USTL_UTIL_NONCOPYABLE_HPP
