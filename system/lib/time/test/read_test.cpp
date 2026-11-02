/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/time_values.hpp>

#include <cstdio>

#include <mtl/scale.hpp>

namespace {
auto fail(char const *const what) -> int {
    // A host test has a console. The message is the mismatch.
    std::fprintf(stderr, "time values: %s\n", what);
    return 1;
}
} // namespace

auto main() -> int {
    static_assert(offsetof(ours::TimeValues, counter) == 48);
    static_assert(sizeof(ours::TimeValues) == 56);

    ours::TimeValues values{};
    values.nanoseconds = 1000;
    values.ticks = 50;
    values.numerator = 10;
    values.denominator = 1;
    if (ours::monotonic_from(values, 50) != 1000) {
        return fail("a sample at the anchor is the anchor");
    }
    if (ours::monotonic_from(values, 53) != 1030) {
        return fail("three ticks at 10 ns");
    }

    values.numerator = 1;
    values.denominator = 2;
    values.nanoseconds = 0;
    values.ticks = 0;
    if (ours::monotonic_from(values, 1) != 1) {
        return fail("1/2 of a tick rounds up");
    }
    if (ours::monotonic_from(values, 2) != 1) {
        return fail("2/2 of a tick is one");
    }

    // A counter that has wrapped under a real ratio does not fit in an
    // Instant. The scale itself is the kernel's; the sum saturates.
    values.numerator = 50000000;
    values.denominator = 715909;
    values.ticks = 10;
    values.nanoseconds = 42;
    auto const wrapped = u64{4};
    auto const delta = wrapped - values.ticks;
    auto const scaled = mtl::scale<mtl::RoundOption::Up, ustl::NumericLimits<u64>::max(), u64, u32>(
        delta, values.numerator, values.denominator);
    auto const sum = static_cast<__int128>(values.nanoseconds) + static_cast<__int128>(scaled);
    auto want = ustl::NumericLimits<ours::Instant>::max();
    if (sum <= ustl::NumericLimits<ours::Instant>::max()) {
        want = static_cast<ours::Instant>(sum);
    }
    if (ours::monotonic_from(values, wrapped) != want) {
        return fail("wrapped delta disagrees with the kernel scale");
    }

    values.numerator = 0;
    values.denominator = 1;
    if (ours::monotonic_from(values, wrapped) != values.nanoseconds) {
        return fail("a zero numerator ignores a wrapped counter");
    }

    values.denominator = 0;
    if (ours::monotonic_from(values, 99) != values.nanoseconds) {
        return fail("a zero denominator does not scale");
    }
    return 0;
}
