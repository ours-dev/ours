/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

// Wire-format contract of the sampler's dump: the host prof-fold tool parses the
// serial export against these exact sizes, offsets and magics, so the test
// pins them.

#include <thread_sampler/format.hpp>

#include <string_view>

#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

namespace {

auto hex_encode(char const *bytes, usize n) -> std::string {
    static constexpr char DIGITS[17] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (usize i = 0; i < n; ++i) {
        auto const byte = static_cast<u8>(bytes[i]);
        out.push_back(DIGITS[byte >> 4]);
        out.push_back(DIGITS[byte & 0xF]);
    }
    return out;
}

auto hex_decode(std::string_view text, char *out) -> bool {
    if (text.size() % 2 != 0) {
        return false;
    }
    auto const nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    for (usize i = 0; i < text.size(); i += 2) {
        int const hi = nibble(text[i]);
        int const lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i / 2] = static_cast<char>((hi << 4) | lo);
    }
    return true;
}

TEST(FormatLayout, RecordAndHeaderSizesArePinned) {
    static_assert(sampler::MAX_STACK_DEPTH == 16);
    static_assert(sizeof(sampler::SampleRecord) == 160);
    static_assert(sizeof(sampler::DumpHeader) == 40);

    EXPECT_EQ(offsetof(sampler::SampleRecord, magic), 0u);
    EXPECT_EQ(offsetof(sampler::SampleRecord, depth), 4u);
    EXPECT_EQ(offsetof(sampler::SampleRecord, flags), 5u);
    EXPECT_EQ(offsetof(sampler::SampleRecord, cpu), 6u);
    EXPECT_EQ(offsetof(sampler::SampleRecord, tid), 8u);
    EXPECT_EQ(offsetof(sampler::SampleRecord, timestamp), 16u);
    EXPECT_EQ(offsetof(sampler::SampleRecord, pid), 24u);
    EXPECT_EQ(offsetof(sampler::SampleRecord, ips), 32u);

    EXPECT_EQ(offsetof(sampler::DumpHeader, magic), 0u);
    EXPECT_EQ(offsetof(sampler::DumpHeader, version), 4u);
    EXPECT_EQ(offsetof(sampler::DumpHeader, header_size), 6u);
    EXPECT_EQ(offsetof(sampler::DumpHeader, record_size), 8u);
    EXPECT_EQ(offsetof(sampler::DumpHeader, num_records), 12u);
    EXPECT_EQ(offsetof(sampler::DumpHeader, num_dropped), 16u);
    EXPECT_EQ(offsetof(sampler::DumpHeader, num_cpus), 20u);
    EXPECT_EQ(offsetof(sampler::DumpHeader, load_base), 24u);
    EXPECT_EQ(offsetof(sampler::DumpHeader, num_unmatched), 32u);
}

TEST(FormatLayout, MagicsMatchTheDocumentedTags) {
    // Little-endian: "SAMP" reads as 0x504D4153, "OPRF" as 0x4652504F.
    EXPECT_EQ(sampler::SAMPLE_MAGIC, 0x504D'4153u);
    EXPECT_EQ(sampler::DUMP_MAGIC, 0x4652'504Fu);
    EXPECT_EQ(sampler::DUMP_VERSION, 2);
}

TEST(FormatLayout, HexLineRoundTrip) {
    sampler::SampleRecord record = {};
    record.magic = sampler::SAMPLE_MAGIC;
    record.depth = 3;
    record.flags = sampler::RecordFlags::Truncated;
    record.cpu = 7;
    record.tid = 42;
    record.timestamp = 0x1122334455667788;
    for (usize i = 0; i < record.depth; ++i) {
        record.ips[i] = 0xFFFF'8000'0000'0000 + i * 0x10;
    }

    auto const encoded = hex_encode(reinterpret_cast<char const *>(&record), sizeof(record));
    EXPECT_EQ(encoded.size(), sizeof(record) * 2);

    sampler::SampleRecord decoded = {};
    ASSERT_TRUE(hex_decode(encoded, reinterpret_cast<char *>(&decoded)));
    EXPECT_EQ(std::memcmp(&record, &decoded, sizeof(record)), 0);
}

TEST(FormatLayout, HexDecodeRejectsGarbage) {
    char out[sizeof(sampler::SampleRecord)];
    EXPECT_FALSE(hex_decode("abc", out));          // odd length
    EXPECT_FALSE(hex_decode("zz", out));           // non-hex
    EXPECT_TRUE(hex_decode("deadbeef", out + 16)); // 4 bytes into scratch
}

} // namespace
