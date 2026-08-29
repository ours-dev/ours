/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_MEM_ADDRESS_OF_HPP
#define USTL_MEM_ADDRESS_OF_HPP 1

#include <memory>

namespace ustl::mem {
template <typename T>
constexpr auto address_of(T &value) noexcept -> T * {
    return std::addressof(value);
}
} // namespace ustl::mem

#endif // USTL_MEM_ADDRESS_OF_HPP
