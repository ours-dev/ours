/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "lz4_block.hpp"

#include <obi/compression.hpp>

#include <algorithm>

#include <lz4.h>
#include <lz4hc.h>

namespace obit {
auto lz4_compress_block(std::span<u8 const> const input)
    -> ustl::Result<std::vector<u8>, ours::Status> {
    if (input.size() > static_cast<usize>(LZ4_MAX_INPUT_SIZE)) {
        return ustl::err(ours::Status::OutOfRange);
    }
    auto const input_size = static_cast<int>(input.size());

    std::vector<u8> output(static_cast<usize>(LZ4_compressBound(input_size)));
    auto const written = LZ4_compress_HC(reinterpret_cast<char const *>(input.data()),
                                         reinterpret_cast<char *>(output.data()), input_size,
                                         static_cast<int>(output.size()), LZ4HC_CLEVEL_DEFAULT);
    // LZ4_compressBound() is enough for any input, so a zero here is an
    // encoder failure, not a buffer that was too small.
    if (written <= 0 && !input.empty()) {
        return ustl::err(ours::Status::BadState);
    }
    output.resize(static_cast<usize>(written));

    // Decoded again by the boot chain's own decoder before it is trusted.
    std::vector<u8> decoded(input.size());
    auto const decoded_size = obi::compression::decompress({output.data(), output.size()},
                                                           {decoded.data(), decoded.size()});
    if (decoded_size.is_err() || decoded_size.unwrap() != input.size() ||
        !std::equal(decoded.begin(), decoded.end(), input.begin(), input.end())) {
        return ustl::err(ours::Status::BadState);
    }
    return ustl::ok(std::move(output));
}
} // namespace obit
