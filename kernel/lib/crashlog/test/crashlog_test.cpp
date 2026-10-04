/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <crashlog/crashlog.hpp>

#include <array>
#include <string>

#include <cstring>

#include <gtest/gtest.h>

namespace {

/// A region as the kernel would hand it over: 8-byte aligned, and full of
/// whatever the RAM held -- here, a pattern that is not a record.
struct Region {
    alignas(8) std::array<std::byte, 256> bytes;

    Region() {
        bytes.fill(std::byte{0xA5});
    }

    auto span() -> std::span<std::byte> {
        return bytes;
    }
};

auto write_record(Region &region, std::string_view text, u64 time) -> void {
    auto writer = crashlog::Writer(region.span());
    writer.append(text);
    writer.seal(time);
}

TEST(Crashlog, RoundTrips) {
    auto region = Region{};
    auto writer = crashlog::Writer(region.span());
    writer.append("first line\n");
    writer.append("second line\n");
    writer.seal(42);
    EXPECT_FALSE(writer.truncated());

    auto const record = crashlog::read(region.span());
    ASSERT_TRUE(record.is_some());
    EXPECT_EQ(record.value().time, 42u);
    EXPECT_EQ(record.value().text, "first line\nsecond line\n");
}

TEST(Crashlog, UnwrittenRegionHoldsNoRecord) {
    auto region = Region{};
    EXPECT_TRUE(crashlog::read(region.span()).is_none());
}

TEST(Crashlog, ClearRemovesTheRecord) {
    auto region = Region{};
    write_record(region, "boom\n", 1);
    crashlog::clear(region.span());
    EXPECT_TRUE(crashlog::read(region.span()).is_none());
}

/// A reset between construction and seal() must not leave the previous
/// record looking valid with new bytes inside it.
TEST(Crashlog, UnsealedWriterLeavesNoRecord) {
    auto region = Region{};
    write_record(region, "old\n", 1);
    auto writer = crashlog::Writer(region.span());
    writer.append("new, never sealed\n");
    EXPECT_TRUE(crashlog::read(region.span()).is_none());
}

TEST(Crashlog, FlippedPayloadBitIsRejected) {
    auto region = Region{};
    write_record(region, "payload\n", 1);
    region.bytes[sizeof(crashlog::Header) + 2] ^= std::byte{0x01};
    EXPECT_TRUE(crashlog::read(region.span()).is_none());
}

TEST(Crashlog, FlippedHeaderBitIsRejected) {
    auto region = Region{};
    write_record(region, "payload\n", 1);
    // The time field: covered by the header CRC and checked by nothing else.
    region.bytes[offsetof(crashlog::Header, time)] ^= std::byte{0x01};
    EXPECT_TRUE(crashlog::read(region.span()).is_none());
}

TEST(Crashlog, OtherVersionIsRejected) {
    auto region = Region{};
    write_record(region, "payload\n", 1);
    auto const version = crashlog::VERSION + 1;
    std::memcpy(region.bytes.data() + offsetof(crashlog::Header, version), &version,
                sizeof(version));
    EXPECT_TRUE(crashlog::read(region.span()).is_none());
}

TEST(Crashlog, LongTextIsCutAtTheRegionEnd) {
    auto region = Region{};
    auto const room = region.bytes.size() - sizeof(crashlog::Header);
    auto const text = std::string(room + 10, 'x');
    auto writer = crashlog::Writer(region.span());
    writer.append(text);
    writer.seal(7);
    EXPECT_TRUE(writer.truncated());

    auto const record = crashlog::read(region.span());
    ASSERT_TRUE(record.is_some());
    EXPECT_EQ(record.value().text.size(), room);
}

TEST(Crashlog, RegionSmallerThanAHeaderHoldsNothing) {
    alignas(8) std::array<std::byte, 8> tiny{};
    auto writer = crashlog::Writer(tiny);
    writer.append("x");
    writer.seal(1);
    EXPECT_TRUE(crashlog::read(tiny).is_none());
}

} // namespace
