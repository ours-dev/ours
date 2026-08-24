/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef LZ4_LZ4_HPP
#define LZ4_LZ4_HPP 1

#include <ours/types.hpp>

namespace lz4 {
/// Encode/decode a raw LZ4 block (without a frame header).  Negative
/// results indicate malformed input or insufficient destination space.
auto compress(void const *src, void *dest, usize src_size, usize dest_size) -> isize;

auto decompress(void const *src, void *dest, usize src_size, usize dest_size) -> isize;

/// Return the decoded size, or zero when the block is malformed.
auto calc_uncompressed_size(void const *src, usize size) -> usize;

} // namespace lz4

#endif // #ifndef LZ4_LZ4_HPP
