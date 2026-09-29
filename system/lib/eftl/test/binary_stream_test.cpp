/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/binary_stream.hpp>

#include <array>

auto main() -> int {
    std::array<u8, 4> const bytes{0x78, 0x56, 0x34, 0x12};

    auto const stream = eftl::BinaryStream::from(bytes.data(), bytes.size());
    if (stream.is_err()) {
        return 1;
    }
    auto const little_endian = stream->read_le<u32>(0);
    if (little_endian.is_err() || little_endian.value() != 0x12345678U) {
        return 2;
    }
    auto const big_endian = stream->read_be<u16>(0);
    if (big_endian.is_err() || big_endian.value() != 0x7856U) {
        return 3;
    }
    if (stream->bytes(3, 2).is_ok()) {
        return 4;
    }
    if (eftl::BinaryStream::from(nullptr, 1).is_err()) {
        return 0;
    }
    return 5;
}
