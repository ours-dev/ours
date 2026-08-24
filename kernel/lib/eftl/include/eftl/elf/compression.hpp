/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_COMPRESSION_HPP
#define EFTL_ELF_COMPRESSION_HPP 1

#include <eftl/elf/encoding.hpp>
#include <eftl/elf/table.hpp>

#include <ustl/option.hpp>

namespace eftl::elf {
enum class CompressionType : u32 {
    Zlib = 1,
    Zstd = 2,
};

template <typename Traits>
class SectionCompressionView final {
  public:
    using Header = typename Traits::Compression;

    [[nodiscard]]
    static auto from(ByteRange bytes, ByteOrder byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<SectionCompressionView> {
        if (bytes.size() < sizeof(Header) || bytes.data() == nullptr) {
            return ustl::NONE;
        }
        Header header{};
        if (byte_order == ByteOrder::Little) {
            header = RecordCodec<Traits, Header>::decode_little(bytes.data());
        } else {
            header = RecordCodec<Traits, Header>::decode_big(bytes.data());
        }
        if (header.ch_type == 0 || header_reserved(header) != 0 ||
            !valid_alignment(header.ch_addralign)) {
            return ustl::NONE;
        }
        return ustl::some(SectionCompressionView(
            header, ByteRange(bytes.data() + sizeof(Header), bytes.size() - sizeof(Header))));
    }

    [[nodiscard]]
    constexpr auto type() const noexcept -> CompressionType {
        return static_cast<CompressionType>(m_header.ch_type);
    }

    [[nodiscard]]
    constexpr auto uncompressed_size() const noexcept -> u64 {
        return static_cast<u64>(m_header.ch_size);
    }

    [[nodiscard]]
    constexpr auto uncompressed_alignment() const noexcept -> u64 {
        return static_cast<u64>(m_header.ch_addralign);
    }

    [[nodiscard]]
    constexpr auto encoded_payload() const noexcept -> ByteRange {
        return m_payload;
    }

  private:
    constexpr SectionCompressionView(Header header, ByteRange payload) noexcept
        : m_header(header), m_payload(payload) {}

    [[nodiscard]]
    static constexpr auto header_reserved(Elf32Chdr const &) noexcept -> u32 {
        return 0;
    }

    [[nodiscard]]
    static constexpr auto header_reserved(Elf64Chdr const &header) noexcept -> u32 {
        return header.ch_reserved;
    }

    template <typename Value>
    [[nodiscard]]
    static constexpr auto valid_alignment(Value alignment) noexcept -> bool {
        return alignment == 0 || (alignment & (alignment - 1)) == 0;
    }

    Header m_header{};
    ByteRange m_payload{};
};
} // namespace eftl::elf

#endif // EFTL_ELF_COMPRESSION_HPP
