/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FDT_FORMAT_HPP
#define FDT_FORMAT_HPP 1

#include <ours/types.hpp>

namespace fdt {

/// What the flattened device tree specification fixes about the blob, and the
/// helpers every reader of it needs. A blob is three blocks behind a 40-byte
/// header: the memory reservation block, the structure block (the tree, as a
/// stream of tokens) and the strings block (property names). Every integer in
/// it is big-endian.

constexpr u32 MAGIC = 0xd00dfeed;

/// The version this reader implements. A blob is readable when the oldest
/// version that can read it (its last_comp_version) is not newer than this,
/// and it must itself be at least this new, because the structure block's size
/// is a header field only from this version on.
constexpr u32 VERSION = 17;

constexpr usize HEADER_SIZE = 40;

/// Header fields, as byte offsets into the blob.
namespace header {
    constexpr usize MAGIC_OFFSET = 0;
    constexpr usize TOTAL_SIZE = 4;
    constexpr usize STRUCTURE_OFFSET = 8;
    constexpr usize STRINGS_OFFSET = 12;
    constexpr usize RESERVATION_OFFSET = 16;
    constexpr usize VERSION_OFFSET = 20;
    constexpr usize LAST_COMPATIBLE_VERSION = 24;
    constexpr usize BOOT_CPU = 28;
    constexpr usize STRINGS_SIZE = 32;
    constexpr usize STRUCTURE_SIZE = 36;
} // namespace header

/// Tokens in the structure block start on a 4-byte boundary.
constexpr usize TOKEN_ALIGN = 4;

/// The reservation block is a run of (address, size) pairs of 8 bytes each,
/// on an 8-byte boundary, ended by a pair of zeros.
constexpr usize RESERVATION_ALIGN = 8;
constexpr usize RESERVATION_ENTRY_SIZE = 16;

/// Nesting beyond this is refused. Real trees are a handful deep; the limit
/// exists so that a hostile blob cannot make the reader's bookkeeping
/// unbounded.
constexpr usize MAX_DEPTH = 32;

enum class Token : u32 {
    BeginNode = 1,
    EndNode = 2,
    Prop = 3,
    Nop = 4,
    End = 9,
};

constexpr auto load_be32(u8 const *const bytes) -> u32 {
    return (u32{bytes[0]} << 24) | (u32{bytes[1]} << 16) | (u32{bytes[2]} << 8) | u32{bytes[3]};
}

constexpr auto load_be64(u8 const *const bytes) -> u64 {
    return (u64{load_be32(bytes)} << 32) | u64{load_be32(bytes + 4)};
}

constexpr auto align_up(usize const value, usize const alignment) -> usize {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace fdt

#endif // FDT_FORMAT_HPP
