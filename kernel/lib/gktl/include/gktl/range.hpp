/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_RANGE_HPP
#define GKTL_RANGE_HPP 1

#include <ours/compiler.hpp>
#include <ours/types.hpp>
#include <algorithm>
namespace gktl {
/// A range is a half-open interval [start, end).
template <typename T>
struct Range {
    FORCE_INLINE constexpr auto length() const -> usize {
        return end - start;
    }

    FORCE_INLINE constexpr auto is_empty() const -> bool {
        return start == end;
    }

    FORCE_INLINE constexpr auto contains(T t) const -> bool {
        return t >= start && t < end;
    }

    FORCE_INLINE constexpr auto overlaps(T other_start, T other_end) const -> bool {
        return (start < other_end) && (end > other_start);
    }

    FORCE_INLINE constexpr auto left_difference_with(Range<T> const &other) const -> Range<T> {
        if (overlaps(other) && start < other.start) {
            return Range<T>{start, other.start};
        }

        // If no valid left difference, return an invalid range (or handle as needed)
        return Range<T>{start, start};
    }

    FORCE_INLINE constexpr auto right_difference_with(Range<T> const &other) const -> Range<T> {
        if (overlaps(other) && end > other.end) {
            return Range<T>{other.end, end};
        }

        // If no valid right difference, return an invalid range (or handle as needed)
        return Range<T>{end, end};
    }

    FORCE_INLINE constexpr auto intersection_with(Range<T> const &other) const -> Range<T> {
        if (overlaps(other)) {
            return Range<T>{std::max(start, other.start),
                            std::min(end, other.end)};
        }

        // If no valid intersection, return an invalid range (or handle as needed)
        return Range<T>{end, end};
    }

    FORCE_INLINE constexpr auto union_with(Range<T> const &other) const -> Range<T> {
        if (overlaps(other)) {
            return Range<T>{std::min(start, other.start),
                            std::max(end, other.end)};
        }

        // If no overlap, return an invalid range (or handle as needed)
        return Range<T>{end, end};
    }

    FORCE_INLINE constexpr auto roundup(T b) -> void {}

    T start;
    T end;
};

template <typename T>
Range(T start, T end) -> Range<T>;

template <typename T>
constexpr auto make_range(T start, T end) -> Range<T> {
    return Range<T>{start, end};
}

} // namespace gktl

#endif // #ifndef KTL_RANGE_HPP