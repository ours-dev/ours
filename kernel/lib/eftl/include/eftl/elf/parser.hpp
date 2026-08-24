/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_PARSER_HPP
#define EFTL_ELF_PARSER_HPP 1

#include <eftl/binary_stream.hpp>
#include <eftl/elf/binary.hpp>

#include <utility>

#include <cstring>

#include <ustl/result.hpp>

namespace eftl::elf {
class Parser final {
  public:
    template <typename Traits>
    [[nodiscard]]
    static auto parse_as(void const *raw, usize size)
        -> ustl::Result<FileImpl<Traits>, ParseError> {
        return FileImpl<Traits>::from_raw(raw, size);
    }

    [[nodiscard]]
    static auto parse(void const *raw, usize size) -> ustl::Result<Binary, ParseError> {
        auto const stream_result = BinaryStream::from(raw, size);
        if (stream_result.is_err()) {
            return ustl::err(ParseError::InvalidHeader);
        }

        auto const ident_result = stream_result->bytes(0, 7);
        if (ident_result.is_err()) {
            return ustl::err(ParseError::InvalidHeader);
        }
        auto const *ident = ident_result->data();
        if (std::memcmp(ident, K_ELF_MAGIC, sizeof(K_ELF_MAGIC)) != 0) {
            return ustl::err(ParseError::InvalidHeader);
        }
        if (ident[4] != Traits32::K_ELF_CLASS && ident[4] != Traits64::K_ELF_CLASS) {
            return ustl::err(ParseError::InvalidHeader);
        }
        if (ident[6] != 1) {
            return ustl::err(ParseError::InvalidHeader);
        }
        if (ident[5] != 1 && ident[5] != 2) {
            return ustl::err(ParseError::InvalidHeader);
        }

        if (ident[4] == Traits32::K_ELF_CLASS) {
            return parse_variant<Traits32>(raw, size);
        }
        return parse_variant<Traits64>(raw, size);
    }

  private:
    template <typename Traits>
    [[nodiscard]]
    static auto parse_variant(void const *raw, usize size)
        -> ustl::Result<Binary, ParseError> {
        auto file_result = parse_as<Traits>(raw, size);
        if (file_result.is_err()) {
            return ustl::err(std::move(file_result).unwrap_err());
        }
        auto const image = BasicImage<Traits>::from(file_result.value());
        if (image.is_none()) {
            return ustl::err(ParseError::NotLoadable);
        }
        return ustl::ok(Binary{Binary::FileVariant{std::move(file_result).unwrap()}});
    }
};
} // namespace eftl::elf

#endif // EFTL_ELF_PARSER_HPP
