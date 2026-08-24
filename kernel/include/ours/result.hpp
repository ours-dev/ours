/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_RESULT_HPP
#define OURS_RESULT_HPP 1

#include <ours/status.hpp>
#include <ustl/result.hpp>

namespace ours {
    using ustl::err;
    using ustl::ok;

    template <typename T>
    using Result = ustl::Result<T, Status>;

} // namespace ours

#endif // OURS_RESULT_HPP
