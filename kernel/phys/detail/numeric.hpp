/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_DETAIL_NUMERIC_HPP
#define OURS_PHYS_DETAIL_NUMERIC_HPP 1

#include <ours/mm/cfg.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>

#include <limits>
#include <type_traits>

/// Numeric helpers shared by the phys loader's translation units.  Private
/// to kernel/phys (included by relative path, never published through
/// BUILD.gn's :headers); eftl keeps its own copies of these because it sits
/// below phys in the layer stack and must not reach up into ours/phys.
namespace ours::phys::detail {

/// Widen an unsigned value to usize, reporting overflow as NONE instead of
/// truncating.  (Carried here from byte-identical copies in elf-image.cpp
/// and elf-image-mapping.cpp.)
template <typename Value>
[[nodiscard]]
constexpr auto checked_to_usize(Value value) noexcept -> ustl::Option<usize> {
    static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);
    if constexpr (sizeof(Value) > sizeof(usize)) {
        if (value > static_cast<Value>(std::numeric_limits<usize>::max())) {
            return ustl::NONE;
        }
    }
    return ustl::some(static_cast<usize>(value));
}

/// The whole pages that cover `byte_count`.  NONE for zero (there is
/// nothing to map) and for a count whose page arithmetic would overflow.
[[nodiscard]]
constexpr auto page_count(usize byte_count) noexcept -> ustl::Option<usize> {
    constexpr auto PAGE_BYTES = static_cast<usize>(PAGE_SIZE);
    if (byte_count == 0) {
        return ustl::NONE;
    }
    auto const whole_pages = byte_count / PAGE_BYTES;
    if (byte_count % PAGE_BYTES == 0) {
        return ustl::some(static_cast<usize>(whole_pages));
    }
    if (whole_pages == std::numeric_limits<usize>::max()) {
        return ustl::NONE;
    }
    return ustl::some(whole_pages + 1);
}

} // namespace ours::phys::detail

#endif // #ifndef OURS_PHYS_DETAIL_NUMERIC_HPP
