/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OBI_COMPRESSION_HPP
#define OBI_COMPRESSION_HPP 1

#include <limits>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace obi::compression {
using Bytes = ustl::views::Span<u8 const>;
using MutableBytes = ustl::views::Span<u8>;
using Result = ustl::Result<usize, ours::Status>;

// This is the raw LZ4 block format (the same sequence format used by
// LZ4_decompress_safe), without a frame header.  The encoder below emits
// literal-only sequences: it compresses nothing, and is kept because the OBI
// tests build their inputs with it.  The boot image itself is compressed by
// obit with upstream LZ4 (kernel/lib/obi/cli/lz4_block.hpp, //vendor/lz4);
// the decoder further down reads both.
[[nodiscard]]
inline auto compressed_size(usize input_size) -> Result {
    auto const extension = input_size < 15 ? usize{0} : (input_size - 15) / 255 + 1;
    if (input_size > std::numeric_limits<usize>::max() - extension - 1) {
        return ustl::err(ours::Status::OutOfRange);
    }
    return ustl::ok(usize{1} + extension + input_size);
}

[[nodiscard]]
inline auto compress_literals(Bytes input, MutableBytes output) -> Result {
    auto const required = compressed_size(input.size());
    if (required.is_err()) {
        return ustl::err(required.unwrap_err());
    }
    if (output.data() == nullptr || output.size() < required.unwrap()) {
        return ustl::err(output.data() == nullptr ? ours::Status::InvArgs : ours::Status::NoSpace);
    }

    auto const literal_length = input.size();
    auto const token_length = literal_length < 15 ? literal_length : 15;
    usize output_offset = 0;
    output[output_offset++] = static_cast<u8>(token_length << 4);
    if (literal_length >= 15) {
        auto remaining = literal_length - 15;
        while (remaining >= 255) {
            output[output_offset++] = 255;
            remaining -= 255;
        }
        output[output_offset++] = static_cast<u8>(remaining);
    }
    for (usize index = 0; index < input.size(); ++index) {
        output[output_offset++] = input[index];
    }
    return ustl::ok(output_offset);
}

/// Return the number of bytes a raw LZ4 block produces without writing a
/// destination buffer.  This makes a declared memory_size verifiable when
/// the container is parsed, before a phys-stage consumer allocates it.
[[nodiscard]]
inline auto decompressed_size(Bytes input) -> Result {
    if (input.data() == nullptr && input.size() != 0) {
        return ustl::err(ours::Status::InvArgs);
    }

    usize input_offset = 0;
    usize output_size = 0;
    while (input_offset < input.size()) {
        auto const token = input[input_offset++];

        auto literal_length = static_cast<usize>(token >> 4);
        if (literal_length == 15) {
            for (;;) {
                if (input_offset >= input.size()) {
                    return ustl::err(ours::Status::InvArgs);
                }
                auto const extension = input[input_offset++];
                if (literal_length > std::numeric_limits<usize>::max() - extension) {
                    return ustl::err(ours::Status::OutOfRange);
                }
                literal_length += extension;
                if (extension != 255) {
                    break;
                }
            }
        }
        if (literal_length > input.size() - input_offset ||
            output_size > std::numeric_limits<usize>::max() - literal_length) {
            return ustl::err(literal_length > input.size() - input_offset
                                 ? ours::Status::InvArgs
                                 : ours::Status::OutOfRange);
        }
        input_offset += literal_length;
        output_size += literal_length;

        if (input_offset == input.size()) {
            break;
        }
        if (input.size() - input_offset < 2) {
            return ustl::err(ours::Status::InvArgs);
        }
        auto const offset = static_cast<usize>(static_cast<u16>(input[input_offset]) |
                                               static_cast<u16>(input[input_offset + 1]) << 8);
        input_offset += 2;
        if (offset == 0 || offset > output_size) {
            return ustl::err(ours::Status::InvArgs);
        }

        auto match_length = static_cast<usize>(token & 0x0F) + 4;
        if ((token & 0x0F) == 0x0F) {
            for (;;) {
                if (input_offset >= input.size()) {
                    return ustl::err(ours::Status::InvArgs);
                }
                auto const extension = input[input_offset++];
                if (match_length > std::numeric_limits<usize>::max() - extension) {
                    return ustl::err(ours::Status::OutOfRange);
                }
                match_length += extension;
                if (extension != 255) {
                    break;
                }
            }
        }
        if (output_size > std::numeric_limits<usize>::max() - match_length) {
            return ustl::err(ours::Status::OutOfRange);
        }
        output_size += match_length;
    }
    return ustl::ok(output_size);
}

[[nodiscard]]
inline auto decompress(Bytes input, MutableBytes output) -> Result {
    if (input.data() == nullptr && input.size() != 0) {
        return ustl::err(ours::Status::InvArgs);
    }
    if (output.data() == nullptr && output.size() != 0) {
        return ustl::err(ours::Status::InvArgs);
    }

    usize input_offset = 0;
    usize output_offset = 0;
    while (input_offset < input.size()) {
        auto const token = input[input_offset++];

        auto literal_length = static_cast<usize>(token >> 4);
        if (literal_length == 15) {
            for (;;) {
                if (input_offset >= input.size()) {
                    return ustl::err(ours::Status::InvArgs);
                }
                auto const extension = input[input_offset++];
                if (literal_length > std::numeric_limits<usize>::max() - extension) {
                    return ustl::err(ours::Status::OutOfRange);
                }
                literal_length += extension;
                if (extension != 255) {
                    break;
                }
            }
        }
        if (literal_length > input.size() - input_offset ||
            literal_length > output.size() - output_offset) {
            return ustl::err(literal_length > input.size() - input_offset ? ours::Status::InvArgs
                                                                          : ours::Status::NoSpace);
        }
        for (usize index = 0; index < literal_length; ++index) {
            output[output_offset++] = input[input_offset++];
        }

        // A final sequence may contain literals only.  Otherwise it has
        // a two-byte little-endian match offset and a match length.
        if (input_offset == input.size()) {
            break;
        }
        if (input.size() - input_offset < 2) {
            return ustl::err(ours::Status::InvArgs);
        }
        auto const offset = static_cast<usize>(static_cast<u16>(input[input_offset]) |
                                               static_cast<u16>(input[input_offset + 1]) << 8);
        input_offset += 2;
        if (offset == 0 || offset > output_offset) {
            return ustl::err(ours::Status::InvArgs);
        }

        auto match_length = static_cast<usize>(token & 0x0F) + 4;
        if ((token & 0x0F) == 0x0F) {
            for (;;) {
                if (input_offset >= input.size()) {
                    return ustl::err(ours::Status::InvArgs);
                }
                auto const extension = input[input_offset++];
                if (match_length > std::numeric_limits<usize>::max() - extension) {
                    return ustl::err(ours::Status::OutOfRange);
                }
                match_length += extension;
                if (extension != 255) {
                    break;
                }
            }
        }
        if (match_length > output.size() - output_offset) {
            return ustl::err(ours::Status::NoSpace);
        }
        auto const match_offset = output_offset - offset;
        for (usize index = 0; index < match_length; ++index) {
            output[output_offset++] = output[match_offset + index % offset];
        }
    }
    return ustl::ok(output_offset);
}
} // namespace obi::compression

#endif // OBI_COMPRESSION_HPP
