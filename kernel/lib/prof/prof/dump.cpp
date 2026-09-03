/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prof/arch.hpp>
#include <prof/format.hpp>
#include <prof/sampler.hpp>

#include <array>

namespace prof {
namespace {
    constexpr char HEX_DIGITS[17] = "0123456789abcdef";

    auto encode_hex(char const *bytes, usize n, char *out) -> void {
        for (usize i = 0; i < n; ++i) {
            auto const byte = static_cast<u8>(bytes[i]);
            out[i * 2] = HEX_DIGITS[byte >> 4];
            out[i * 2 + 1] = HEX_DIGITS[byte & 0xF];
        }
    }

    /// Builds one full `<tag> <hex>\r\n` line on the stack (a record line is
    /// at most 4 + 1 + 2*152 + 2 = 311 bytes) and hands it to
    /// `arch::debug_write` in a single call.
    template <usize N>
    auto write_line(char const (&tag)[N], void const *bytes, usize len) -> void {
        constexpr usize TAG_LEN = N - 1; // exclude the string literal's NUL.

        std::array<char, 512> line{};
        usize pos = 0;
        for (usize i = 0; i < TAG_LEN; ++i) {
            line[pos++] = tag[i];
        }
        line[pos++] = ' ';
        encode_hex(static_cast<char const *>(bytes), len, &line[pos]);
        pos += len * 2;
        line[pos++] = '\r';
        line[pos++] = '\n';
        arch::debug_write(line.data(), pos);
    }
} // namespace

auto Sampler::num_cpus() const -> u32 {
    return m_num_cpus;
}

auto Sampler::buffer_stats(u32 cpu) const -> BufferStats {
    auto *buffer = buffer_for(cpu);
    if (buffer == nullptr) {
        return BufferStats{};
    }
    return BufferStats{
        .retained = buffer->size(),
        .dropped = buffer->dropped(),
        .total = buffer->total(),
    };
}

auto Sampler::export_dump() const -> void {
    u32 records = 0;
    u32 dropped = 0;
    for (u32 cpu = 0; cpu < m_num_cpus; ++cpu) {
        auto const stats = buffer_stats(cpu);
        records += static_cast<u32>(stats.retained);
        dropped += static_cast<u32>(stats.dropped);
    }

    DumpHeader header = {};
    header.magic = DUMP_MAGIC;
    header.version = DUMP_VERSION;
    header.header_size = sizeof(DumpHeader);
    header.record_size = sizeof(SampleRecord);
    header.num_records = records;
    header.num_dropped = dropped;
    header.load_base = m_image_base;
    header.num_cpus = m_num_cpus;

    write_line(DUMP_HEADER_TAG, &header, sizeof(header));

    for (u32 cpu = 0; cpu < m_num_cpus; ++cpu) {
        auto *buffer = buffer_for(cpu);
        if (buffer == nullptr) {
            continue;
        }
        auto const count = buffer->size();
        for (usize i = 0; i < count; ++i) {
            auto const &record = buffer->slot(i);
            if (record.magic != SAMPLE_MAGIC) {
                continue;
            }
            write_line(DUMP_RECORD_TAG, &record, sizeof(record));
        }
    }
}

} // namespace prof
