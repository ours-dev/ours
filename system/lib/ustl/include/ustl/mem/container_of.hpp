/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_MEM_CONTAINER_OF_HPP
#define USTL_MEM_CONTAINER_OF_HPP 1

#include <cstddef>

namespace ustl::mem {

template <typename Container, typename Member>
inline auto offset_of(Member Container::*member) noexcept -> std::ptrdiff_t {
    auto const *owner = static_cast<Container const *>(nullptr);
    auto const *field = &(owner->*member);
    return reinterpret_cast<char const *>(field) - reinterpret_cast<char const *>(owner);
}

template <typename Container, typename Member>
inline auto container_of(Member *member, Member Container::*field) noexcept -> Container * {
    return reinterpret_cast<Container *>(reinterpret_cast<char *>(member) - offset_of(field));
}

template <typename Container, typename Member>
inline auto container_of(Member const *member, Member Container::*field) noexcept -> Container const * {
    return reinterpret_cast<Container const *>(reinterpret_cast<char const *>(member) - offset_of(field));
}

}  // namespace ustl::mem

#endif  // USTL_MEM_CONTAINER_OF_HPP
