/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/bitset.hpp>

#include <type_traits>

#include <cassert>
#include <cstdint>

namespace {

using Small = ustl::BitSet<16>;
using Wide = ustl::BitSet<80>;
using Typed = ustl::BitSet<16, std::uint32_t>;

static_assert(Small::BIT_COUNT == 16);
static_assert(Small::size() == 16);
static_assert(std::is_trivially_copyable_v<Small>);
static_assert(std::is_trivially_copyable_v<Wide>);

constexpr auto test_single_word() -> bool {
    auto mask = Small::of(0);
    mask.set(7).set(15).set(16);
    if (mask.count() != 3) {
        return false;
    }
    if (!mask.test(0) || !mask.test(7) || !mask.test(15)) {
        return false;
    }
    if (mask.test(16) || mask[16]) {
        return false;
    }

    auto const selected = Small::of(7) | Small::of(9);
    auto const intersection = mask & selected;
    if (intersection.count() != 1 || !intersection.test(7) || intersection.test(0)) {
        return false;
    }

    mask.set(7, false);
    if (mask.count() != 2 || mask.test(7) || !mask.any()) {
        return false;
    }

    mask.reset();
    return mask.none();
}

constexpr auto test_multi_word_span() -> bool {
    auto mask = Wide{};
    mask.set(63).set(64).set(79);
    return mask.count() == 3 && mask.test(63) && mask.test(64) && mask.test(79);
}

constexpr auto test_multi_word_trim() -> bool {
    auto overflow = Wide{static_cast<std::uint64_t>(~0ULL)};
    return overflow.count() == 64 && !overflow.test(64);
}

constexpr auto test_multi_word_all() -> bool {
    auto all_set = Wide{};
    all_set.set();
    return all_set.all() && all_set.count() == 80 && (all_set ^ all_set).none();
}

constexpr auto test_multi_word_flip() -> bool {
    auto flipped = ~Wide{};
    if (!flipped.all() || flipped.count() != 80) {
        return false;
    }
    flipped.flip(70);
    return !flipped.test(70) && !flipped.all() && flipped.count() == 79;
}

constexpr auto test_equality_and_range() -> bool {
    auto left = Small::of(3);
    auto right = Small::of(3);
    if (left != right) {
        return false;
    }
    right.set(4);
    if (left == right) {
        return false;
    }
    return left.test_range(3, 1) && !left.test_range(3, 2) && !left.test_range(16, 1);
}

static_assert(test_single_word());
static_assert(test_multi_word_span());
static_assert(test_multi_word_trim());
static_assert(test_multi_word_all());
static_assert(test_multi_word_flip());
static_assert(test_equality_and_range());

} // namespace

auto main() -> int {
    auto mask = Typed::of(0);
    mask.set(7).set(15).set(16);
    assert(mask.count() == 3);
    assert(mask.test(0) && mask.test(7) && mask.test(15));
    assert(!mask.test(16));

    auto visited = std::size_t{0};
    mask.for_each([&visited](std::uint32_t) { visited += 1; });
    assert(visited == mask.count());

    auto selected = Typed::of(7) | Typed::of(9);
    auto const intersection = mask & selected;
    assert(intersection.count() == 1 && intersection.test(7));

    mask.reset();
    assert(mask.none());

    auto wide = Wide{};
    wide.set(64).set(79);
    auto wide_visited = std::size_t{0};
    wide.for_each([&wide_visited](std::size_t index) {
        wide_visited += 1;
        assert(index == 64 || index == 79);
    });
    assert(wide_visited == 2);
}
