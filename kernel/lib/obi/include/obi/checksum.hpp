/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OBI_CHECKSUM_HPP
#define OBI_CHECKSUM_HPP 1

#include <limits>

#include <ours/types.hpp>

namespace obi {
// CRC-32/ISO-HDLC, matching the checksum used by ZBI-style images.
[[nodiscard]]
inline auto crc32(u8 const *data, usize size, usize zero_offset = std::numeric_limits<usize>::max(),
                  usize zero_size = 0) -> u32 {
    auto checksum = u32{0xFFFFFFFFu};
    for (usize index = 0; index < size; ++index) {
        auto byte = data[index];
        if (index >= zero_offset && index - zero_offset < zero_size) {
            byte = 0;
        }
        checksum ^= byte;
        for (u32 bit = 0; bit < 8; ++bit) {
            checksum = (checksum & 1u) != 0 ? (checksum >> 1u) ^ 0xEDB88320u : checksum >> 1u;
        }
    }
    return checksum ^ 0xFFFFFFFFu;
}
} // namespace obi

#endif // OBI_CHECKSUM_HPP
