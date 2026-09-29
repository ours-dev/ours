/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_ELF_SEGMENT_HPP
#define EFTL_ELF_SEGMENT_HPP 1

#include <eftl/elf/config.hpp>
#include <eftl/elf/details/segment.hpp>
#include <eftl/elf/enums.hpp>
#include <eftl/elf/traits.hpp>

#include <memory>

#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/views/span.hpp>

namespace eftl::elf {
template <SegType>
struct SegTypeMatcher {};

template <typename Derived, SegType... Type>
struct SegmentObserver {};

template <typename Memory, typename Segment, typename Observer, SegType... Types>
FORCE_INLINE [[nodiscard]]
constexpr auto decode_segment(Memory &&memory, Segment const &segment,
                              SegmentObserver<Observer, Types...> &observer) -> bool {
    auto is_ok = true;
    auto const invoke_observe = [&](auto const &tag) -> bool {
        is_ok = static_cast<Observer &>(observer).observe(memory, segment, tag);
        return true;
    };

    ((segment.type() == Types && invoke_observe(SegTypeMatcher<Types>())) || ...);
    return is_ok;
}

template <typename Memory, typename Range, typename... Observer>
[[nodiscard]]
auto decode_segments(Memory &&memory, Range const &segments, Observer &&...observers) -> bool {
    for (auto const segment: segments) {
        if ((!decode_segment(memory, segment, observers) || ...)) {
            return false;
        }
    }

    return (observers.finish() && ...);
}

/// This observer class only watch a type of segment.
template <typename Traits, SegType Type>
struct SegmentSingletonObserver final
    : public SegmentObserver<SegmentSingletonObserver<Traits, Type>, Type> {
    using Segment = typename Traits::Segment;

    SegmentSingletonObserver(Segment &segment) : m_segment(std::addressof(segment)) {}

    template <typename Memory>
    FORCE_INLINE [[nodiscard]]
    constexpr auto observe(Memory &&memory, Segment const &segment, SegTypeMatcher<Type>) -> bool {
        (void)memory;
        if (!m_segment) {
            return false;
        }

        *m_segment = segment;
        return true;
    }

    FORCE_INLINE [[nodiscard]]
    constexpr auto finish() -> bool {
        return true;
    }

    Segment *m_segment{nullptr};
};

template <typename Traits>
using LoadSegObserver = SegmentSingletonObserver<Traits, SegType::Load>;

template <typename Traits>
using DynSegObserver = SegmentSingletonObserver<Traits, SegType::Dynamic>;

template <typename Traits>
using TlsSegObserver = SegmentSingletonObserver<Traits, SegType::Tls>;

} // namespace eftl::elf

#endif // #ifndef EFTL_ELF_SEGMENT_HPP
