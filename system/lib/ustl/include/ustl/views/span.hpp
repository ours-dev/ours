/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_VIEWS_SPAN_HPP
#define USTL_VIEWS_SPAN_HPP 1

#include <span>

namespace ustl::views {
using std::dynamic_extent;

template <typename T, std::size_t Extent = std::dynamic_extent> using Span = std::span<T, Extent>;

template <typename T> constexpr auto make_span(T *data, std::size_t size) noexcept -> Span<T> {
    return Span<T>(data, size);
}
} // namespace ustl::views

#endif // USTL_VIEWS_SPAN_HPP
