/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_RESULT_HPP
#define KTL_RESULT_HPP 1

#include <ours/status.hpp>

#include <ustl/result.hpp>

namespace ktl {
template <typename T>
using Result = ustl::Result<T, ours::Status>;

using ustl::err;
using ustl::ok;

} // namespace ktl

#endif // #ifndef KTL_RESULT_HPP