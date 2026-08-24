/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_FUNCTION_FN_HPP
#define USTL_FUNCTION_FN_HPP 1

#include <functional>

namespace ustl::function {

template <typename Signature>
using Fn = std::function<Signature>;

} // namespace ustl::function

#endif // USTL_FUNCTION_FN_HPP
