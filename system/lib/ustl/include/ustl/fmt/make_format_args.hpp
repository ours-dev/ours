// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <format>
#include <string_view>
#include <type_traits>

#include <ustl/fmt/format_args.hpp>

namespace ustl::fmt {

using std::make_format_args;

/// Hands one argument to make_format_args() in a shape this build's libc++
/// actually formats.
///
/// A character *array* -- which is what a `char name[24]` local is, and what a
/// stringified macro argument is -- reaches the argument store as an array
/// type rather than as `const char *`, and comes out of the formatter as
/// nothing at all: no diagnostic, no output, just a `{}` that renders empty.
/// A pointer or a string_view formats correctly, so decay the array on the way
/// in. Everything else is forwarded untouched.
///
/// The result may be a prvalue string_view, and make_format_args() requires
/// lvalues, so a caller has to bind it before building the store -- see the
/// lambda in logz4::log() and in ours::panic().
template <typename T>
constexpr auto decay_format_arg(T &&value) -> decltype(auto) {
    using Bare = std::remove_cvref_t<T>;
    if constexpr (std::is_array_v<Bare> && std::is_same_v<std::remove_extent_t<Bare>, char>) {
        return std::string_view(value);
    } else {
        return static_cast<T &&>(value);
    }
}

} // namespace ustl::fmt
