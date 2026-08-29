/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_VARIANT_HPP
#define USTL_VARIANT_HPP 1

#include <variant>

namespace ustl {
template <typename... Types>
using Variant = std::variant<Types...>;

using std::get;
using std::visit;
} // namespace ustl

#endif // USTL_VARIANT_HPP
