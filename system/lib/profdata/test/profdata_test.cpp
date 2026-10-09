/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <profdata/profdata.hpp>

#include <string_view>
#include <vector>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "coverage_example.hpp"
#include <gtest/gtest.h>
#include <profile/InstrProfData.inc>

// A host definition of the panic hook <ours/assert.hpp> ends in. The library
// asserts on the sizes it is given, and a failed ASSERT renders its message and
// hands it here; the link needs the symbol whether or not one ever fails.
namespace ours {
[[noreturn]]
auto vpanic(std::string_view message) -> void {
    std::fprintf(stderr, "PANIC: %.*s\n", static_cast<int>(message.size()), message.data());
    std::abort();
}
} // namespace ours

namespace {

// The header, field for field as the compiler's InstrProfData.inc lists it, so
// that a test can read back what was written.
struct Header {
#define INSTR_PROF_RAW_HEADER(Type, Name, Initializer) Type Name;
#include <profile/InstrProfData.inc>
};

// What the tests use for the profiled binary's identity. The library only
// copies it, so any bytes will do.
constexpr std::byte BUILD_ID[] = {std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}};

// A dump buffer, aligned as the library asks.
class Dump {
  public:
    explicit Dump(usize const size_bytes) : m_words((size_bytes + sizeof(u64) - 1) / sizeof(u64)) {}

    auto bytes() -> std::span<std::byte> {
        return {reinterpret_cast<std::byte *>(m_words.data()), m_words.size() * sizeof(u64)};
    }

  private:
    std::vector<u64> m_words;
};

// The sum of the counters in a copied region, which is what a call to an
// instrumented function adds to.
auto sum_counters(std::span<std::byte const> const counters) -> u64 {
    u64 sum = 0;
    for (usize offset = 0; offset + sizeof(u64) <= counters.size(); offset += sizeof(u64)) {
        u64 value = 0;
        std::memcpy(&value, counters.data() + offset, sizeof(value));
        sum += value;
    }
    return sum;
}

// Whether the compiler counted in single bytes, which the dump's own header says.
auto single_byte_counters(std::span<std::byte const> const dump) -> bool {
    Header header{};
    std::memcpy(&header, dump.data(), sizeof(header));
    return (header.Version & VARIANT_MASK_BYTE_COVERAGE) != 0;
}

TEST(RawProfile, HasSomethingToWriteWhenTheProgramIsInstrumented) {
    profdata::RawProfile const profile{BUILD_ID};
    EXPECT_GT(profile.size_bytes(), usize{sizeof(Header)});
}

TEST(RawProfile, StartsWithTheHeaderAndTheBuildId) {
    profdata::RawProfile const profile{BUILD_ID};
    Dump dump{profile.size_bytes()};
    auto const regions = profile.write_fixed(dump.bytes());
    EXPECT_FALSE(regions.counters.empty());

    Header header{};
    std::memcpy(&header, dump.bytes().data(), sizeof(header));
    EXPECT_EQ(header.Magic, INSTR_PROF_RAW_MAGIC_64);
    // The version word may carry flag bits the compiler sets.
    EXPECT_EQ(header.Version & ~VARIANT_MASKS_ALL, u64{INSTR_PROF_RAW_VERSION});
    // The build id's length, the id, and padding to 8 bytes.
    EXPECT_EQ(header.BinaryIdsSize, sizeof(u64) + 8);

    u64 id_size = 0;
    std::memcpy(&id_size, dump.bytes().data() + sizeof(header), sizeof(id_size));
    EXPECT_EQ(id_size, sizeof(BUILD_ID));
    EXPECT_EQ(std::memcmp(dump.bytes().data() + sizeof(header) + sizeof(id_size), BUILD_ID,
                          sizeof(BUILD_ID)),
              0);
}

TEST(RawProfile, LiveRegionsLieInsideTheDumpInOrder) {
    profdata::RawProfile const profile{BUILD_ID};
    Dump dump{profile.size_bytes()};
    auto const regions = profile.write_fixed(dump.bytes());

    auto const begin = dump.bytes().data();
    auto const end = begin + profile.size_bytes();
    EXPECT_GE(regions.counters.data(), begin);
    EXPECT_LE(regions.counters.data() + regions.counters.size(), regions.bitmap.data());
    EXPECT_LE(regions.bitmap.data() + regions.bitmap.size(), regions.uniform_counters.data());
    EXPECT_LE(regions.uniform_counters.data() + regions.uniform_counters.size(), end);

    // The counters are 64-bit values, so they must start where one can.
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(regions.counters.data()) %
                  profdata::RawProfile::ALIGNMENT,
              0U);
}

TEST(RawProfile, CopiedCountersGrowWithEachCallToInstrumentedCode) {
    profdata::RawProfile const profile{BUILD_ID};
    Dump dump{profile.size_bytes()};
    auto const regions = profile.write_fixed(dump.bytes());
    if (single_byte_counters(dump.bytes())) {
        GTEST_SKIP() << "the sum of counters means something only when they are 64-bit";
    }

    profile.copy_live(regions);
    auto const before = sum_counters(regions.counters);

    constexpr int POSITIVE_CALLS = 5;
    constexpr int NEGATIVE_CALLS = 3;
    for (int i = 0; i < POSITIVE_CALLS; ++i) {
        EXPECT_EQ(coverage_example::classify(1), 1);
    }
    for (int i = 0; i < NEGATIVE_CALLS; ++i) {
        EXPECT_EQ(coverage_example::classify(-1), -1);
    }

    profile.copy_live(regions);
    // Each call counts the function's entry at least once.
    EXPECT_GE(sum_counters(regions.counters) - before, u64{POSITIVE_CALLS + NEGATIVE_CALLS});
}

// Writes the dump to the file named by PROFDATA_TEST_DUMP, when it is set, so
// that llvm-profdata and llvm-cov can be pointed at what this library wrote.
TEST(RawProfile, CanBeWrittenToAFileForLlvmProfdata) {
    char const *const path = std::getenv("PROFDATA_TEST_DUMP");
    if (path == nullptr) {
        GTEST_SKIP() << "PROFDATA_TEST_DUMP is not set";
    }
    profdata::RawProfile const profile{BUILD_ID};
    Dump dump{profile.size_bytes()};
    auto const regions = profile.write_fixed(dump.bytes());
    EXPECT_EQ(coverage_example::classify(7), 1);
    profile.copy_live(regions);

    std::FILE *const file = std::fopen(path, "wb");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(std::fwrite(dump.bytes().data(), 1, profile.size_bytes(), file),
              profile.size_bytes());
    EXPECT_EQ(std::fclose(file), 0);
}

} // namespace
