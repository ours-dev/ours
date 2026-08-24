/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/checked_reader.hpp>

#include <cstdint>

namespace {
// NOLINTBEGIN(readability-identifier-naming)
struct Pair {
    std::uint32_t first;
    std::uint16_t second;
};
// NOLINTEND(readability-identifier-naming)
} // namespace

static_assert(eftl::elf::WireObject<std::uint16_t>);
static_assert(!eftl::elf::WireObject<bool>);
static_assert(!eftl::elf::WireObject<const std::uint16_t>);
static_assert(!eftl::elf::WireObject<volatile std::uint16_t>);
static_assert(!eftl::elf::WireObject<Pair>);

auto main() -> int {
    if (eftl::elf::CheckedReader::from(nullptr, 1).is_ok() ||
        eftl::elf::CheckedReader::from(nullptr, 0).is_err()) {
        return 1;
    }

    std::uint8_t const bytes[] = {1, 2, 3, 4, 5, 6, 7, 8};

    auto const reader_result = eftl::elf::CheckedReader::from(bytes, sizeof(bytes));
    if (reader_result.is_err()) {
        return 2;
    }
    auto const reader = reader_result.unwrap();

    auto range = reader.bytes(2, 3);
    if (range.is_err() || range.unwrap().size() != 3 || range.unwrap().data()[0] != 3) {
        return 3;
    }

    auto out_of_range = reader.bytes(7, 2);
    if (out_of_range.is_ok() || out_of_range.error() != eftl::elf::ReaderError::OutOfRange) {
        return 4;
    }

    auto overflow = reader.array<std::uint32_t>(1, static_cast<usize>(-1));
    if (overflow.is_ok() || overflow.error() != eftl::elf::ReaderError::OutOfRange) {
        return 5;
    }

    auto object = reader.object<std::uint16_t>(2);
    if (object.is_ok() && object.unwrap() == 0x0403) {
        return 0;
    }
    return 6;
}
