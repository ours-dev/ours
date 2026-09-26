/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The byte loops behind mem* and strlen, called as ours::libc::* so that
/// each call really runs them (src/string_impl.hpp says why the C names
/// cannot be tested here). The host's own functions serve as the reference
/// where one is needed: they are what C specifies, and nothing below links
/// ours in their place.
#include <array>

#include <cstring>

#include "../src/string_impl.hpp"
#include <gtest/gtest.h>

namespace {
namespace libc = ours::libc;

/// A buffer whose every byte differs from its neighbours, so that a byte
/// copied from the wrong place shows.
template <usize N>
auto pattern(u8 const seed) -> std::array<u8, N> {
    std::array<u8, N> bytes{};
    for (usize i = 0; i < N; ++i) {
        bytes[i] = static_cast<u8>(seed + i * 7);
    }
    return bytes;
}

auto sign(int const value) -> int {
    return (value > 0) - (value < 0);
}

TEST(Memcpy, CopiesExactlyNBytesAndReturnsTheDestination) {
    auto const source = pattern<16>(1);
    auto destination = pattern<16>(100);
    auto const before = destination;

    EXPECT_EQ(libc::memcpy(destination.data(), source.data(), 5), destination.data());
    for (usize i = 0; i < 5; ++i) {
        EXPECT_EQ(destination[i], source[i]) << "byte " << i;
    }
    for (usize i = 5; i < 16; ++i) {
        EXPECT_EQ(destination[i], before[i]) << "byte " << i << " is past n";
    }
}

TEST(Memcpy, ZeroBytesTouchesNothing) {
    auto destination = pattern<4>(9);
    auto const before = destination;
    EXPECT_EQ(libc::memcpy(destination.data(), nullptr, 0), destination.data());
    EXPECT_EQ(destination, before);
}

/// Every overlap a 32-byte buffer allows, both ways, against the host's
/// memmove on a copy of the same bytes.
TEST(Memmove, MatchesTheReferenceForEveryOverlap) {
    constexpr usize SIZE = 32;
    for (usize from = 0; from < SIZE; ++from) {
        for (usize to = 0; to < SIZE; ++to) {
            for (usize n = 0; n <= SIZE - std::max(from, to); ++n) {
                auto ours = pattern<SIZE>(3);
                auto reference = ours;
                auto *const returned = libc::memmove(ours.data() + to, ours.data() + from, n);
                std::memmove(reference.data() + to, reference.data() + from, n);
                ASSERT_EQ(returned, ours.data() + to);
                ASSERT_EQ(ours, reference) << "from " << from << " to " << to << " n " << n;
            }
        }
    }
}

TEST(Memset, StoresTheLowByteOnlyAndReturnsTheDestination) {
    auto bytes = pattern<8>(5);
    auto const before = bytes;

    EXPECT_EQ(libc::memset(bytes.data(), 0x1ab, 6), bytes.data());
    for (usize i = 0; i < 6; ++i) {
        EXPECT_EQ(bytes[i], 0xab) << "byte " << i;
    }
    EXPECT_EQ(bytes[6], before[6]);
    EXPECT_EQ(bytes[7], before[7]);

    libc::memset(bytes.data(), -1, 3);
    EXPECT_EQ(bytes[0], 0xff);
    EXPECT_EQ(bytes[2], 0xff);
    EXPECT_EQ(bytes[3], 0xab);
}

TEST(Memset, ZeroBytesTouchesNothing) {
    auto bytes = pattern<4>(2);
    auto const before = bytes;
    EXPECT_EQ(libc::memset(bytes.data(), 0, 0), bytes.data());
    EXPECT_EQ(bytes, before);
}

/// The regression the unsigned-char fix is for: with bytes read as signed
/// char, 0x80 compared below 0x01.
TEST(Memcmp, OrdersBytesAsUnsignedChar) {
    u8 const high[] = {0x80};
    u8 const low[] = {0x01};
    EXPECT_GT(libc::memcmp(high, low, 1), 0);
    EXPECT_LT(libc::memcmp(low, high, 1), 0);

    u8 const top[] = {0xff};
    u8 const bottom[] = {0x00};
    EXPECT_GT(libc::memcmp(top, bottom, 1), 0);
    EXPECT_LT(libc::memcmp(bottom, top, 1), 0);
}

TEST(Memcmp, TheFirstDifferenceDecides) {
    u8 const a[] = {'a', 'b', 0x01, 0xff};
    u8 const b[] = {'a', 'b', 0x02, 0x00};
    EXPECT_LT(libc::memcmp(a, b, 4), 0) << "the 0xff after the first difference must not count";
    EXPECT_EQ(libc::memcmp(a, b, 2), 0);
    EXPECT_EQ(libc::memcmp(a, b, 0), 0);
    EXPECT_EQ(libc::memcmp(a, a, 4), 0);
}

/// Every pair of byte values, as a sign against the host's memcmp: C fixes
/// only the sign of the answer, not its size.
TEST(Memcmp, SignMatchesTheReferenceForEveryBytePair) {
    for (unsigned x = 0; x < 256; ++x) {
        for (unsigned y = 0; y < 256; ++y) {
            u8 const a[] = {'=', static_cast<u8>(x)};
            u8 const b[] = {'=', static_cast<u8>(y)};
            ASSERT_EQ(sign(libc::memcmp(a, b, 2)), sign(std::memcmp(a, b, 2)))
                << "x " << x << " y " << y;
        }
    }
}

/// The regression the matching-byte fix is for: every hit used to come back
/// as the start of the buffer.
TEST(Memchr, ReturnsTheMatchingByteNotTheStart) {
    char const text[] = "kernel.select.userboot=shell";
    auto const *const hit = static_cast<char const *>(libc::memchr(text, '=', sizeof(text) - 1));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit - text, 22);
}

TEST(Memchr, FindsTheFirstMatchWithinNAndNothingPastIt) {
    char const text[] = {'a', 'b', 'c', 'b', 'x'};
    EXPECT_EQ(libc::memchr(text, 'b', 5), text + 1);
    EXPECT_EQ(libc::memchr(text, 'x', 5), text + 4) << "a match in the last byte";
    EXPECT_EQ(libc::memchr(text, 'x', 4), nullptr) << "a match just past n";
    EXPECT_EQ(libc::memchr(text, 'z', 5), nullptr);
    EXPECT_EQ(libc::memchr(text, 'a', 0), nullptr);
}

/// |c| is converted to unsigned char first (C17 7.24.5.1), so only its low
/// byte counts.
TEST(Memchr, ComparesTheLowByteOfC) {
    u8 const bytes[] = {0x01, 0x80, 0xff};
    EXPECT_EQ(libc::memchr(bytes, 0x180, 3), bytes + 1);
    EXPECT_EQ(libc::memchr(bytes, -1, 3), bytes + 2);
    EXPECT_EQ(libc::memchr(bytes, 0x101, 3), bytes + 0);
}

TEST(Strlen, CountsUpToTheFirstNul) {
    EXPECT_EQ(libc::strlen(""), 0u);
    EXPECT_EQ(libc::strlen("vmours"), 6u);
    char const embedded[] = {'a', 'b', '\0', 'c', '\0'};
    EXPECT_EQ(libc::strlen(embedded), 2u);

    std::array<char, 4097> longer{};
    longer.fill('a');
    longer.back() = '\0';
    EXPECT_EQ(libc::strlen(longer.data()), 4096u);
}
} // namespace
