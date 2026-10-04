/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OBI_CLI_LZ4_BLOCK_HPP
#define OBI_CLI_LZ4_BLOCK_HPP 1

#include <span>
#include <vector>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

namespace obit {
/// |input| as one raw LZ4 block, with no frame: the format an OBI item marked
/// Compression::Lz4 carries, and the one obi::compression::decompress() reads
/// in phys and userboot.
///
/// Compressed with upstream LZ4's high-compression encoder (LZ4_compress_HC,
/// at LZ4HC_CLEVEL_DEFAULT), vendored at //vendor/lz4. It replaces
/// obi::compression::compress_literals(), which wrote literals only and so
/// never compressed anything (docs/repairs/260925/obi.md, item 3).
///
/// The block is decoded again with obi::compression::decompress(), the
/// decoder the boot chain uses, and compared with |input| before it is
/// returned. Every image the build packs is therefore proven to round-trip
/// through the very code that will read it. BadState if it does not;
/// OutOfRange for an input larger than LZ4_MAX_INPUT_SIZE, since LZ4 takes
/// sizes as int.
[[nodiscard]]
auto lz4_compress_block(std::span<u8 const> input) -> ustl::Result<std::vector<u8>, ours::Status>;
} // namespace obit

#endif // #ifndef OBI_CLI_LZ4_BLOCK_HPP
