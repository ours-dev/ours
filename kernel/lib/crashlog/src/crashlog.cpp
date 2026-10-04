/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <crashlog/crashlog.hpp>

#include <atomic>

#include <cstring>

#include <obi/checksum.hpp>

namespace crashlog {
namespace {
    constexpr auto HEADER_BYTES = sizeof(Header);

    /// Read through memcpy rather than a cast: the region is bytes the kernel
    /// found in RAM, and nothing guarantees a Header object lives there.
    auto load_header(std::span<std::byte const> region) -> Header {
        auto header = Header{};
        std::memcpy(&header, region.data(), sizeof(header));
        return header;
    }

    auto header_crc(Header header) -> u32 {
        header.magic = 0;
        header.header_crc32 = 0;
        return obi::crc32(reinterpret_cast<u8 const *>(&header), sizeof(header));
    }

    auto payload_crc(std::span<std::byte const> payload) -> u32 {
        return obi::crc32(reinterpret_cast<u8 const *>(payload.data()), payload.size());
    }

    /// Stores the magic after every store before it.
    ///
    /// A reset is not another CPU, so no atomic is called for: what must hold
    /// is only that the magic reaches memory after the bytes it vouches for.
    /// x86 makes stores visible in program order, and the caller writes the
    /// cache back after seal(); what is left is the compiler, which could
    /// otherwise move this store above the payload's, and this fence stops it.
    auto store_magic(std::span<std::byte> region, u64 const magic) -> void {
        std::atomic_signal_fence(std::memory_order_release);
        std::memcpy(region.data(), &magic, sizeof(magic));
    }
} // namespace

Writer::Writer(std::span<std::byte> region) : m_region(region) {
    if (m_region.size() < HEADER_BYTES) {
        m_region = {};
        return;
    }
    clear(m_region);
}

auto Writer::append(std::string_view text) -> void {
    if (m_region.empty()) {
        return;
    }
    auto const room = m_region.size() - HEADER_BYTES - m_length;
    auto count = text.size();
    if (count > room) {
        count = room;
        m_truncated = true;
    }
    std::memcpy(m_region.data() + HEADER_BYTES + m_length, text.data(), count);
    m_length += count;
}

auto Writer::seal(u64 const time) -> void {
    if (m_region.empty()) {
        return;
    }
    auto header = Header{
        .magic = 0,
        .version = VERSION,
        .payload_len = static_cast<u32>(m_length),
        .payload_crc32 = payload_crc(m_region.subspan(HEADER_BYTES, m_length)),
        .header_crc32 = 0,
        .time = time,
    };
    header.header_crc32 = header_crc(header);
    std::memcpy(m_region.data(), &header, sizeof(header));
    store_magic(m_region, MAGIC);
}

auto read(std::span<std::byte const> region) -> ustl::Option<Record> {
    if (region.size() < HEADER_BYTES) {
        return ustl::NONE;
    }
    auto const header = load_header(region);
    if (header.magic != MAGIC || header.version != VERSION) {
        return ustl::NONE;
    }
    if (header.header_crc32 != header_crc(header)) {
        return ustl::NONE;
    }
    if (header.payload_len > region.size() - HEADER_BYTES) {
        return ustl::NONE;
    }
    auto const payload = region.subspan(HEADER_BYTES, header.payload_len);
    if (payload_crc(payload) != header.payload_crc32) {
        return ustl::NONE;
    }
    return ustl::some(Record{
        .time = header.time,
        .text = std::string_view(reinterpret_cast<char const *>(payload.data()), payload.size()),
    });
}

auto clear(std::span<std::byte> region) -> void {
    if (region.size() < HEADER_BYTES) {
        return;
    }
    store_magic(region, 0);
}

} // namespace crashlog
