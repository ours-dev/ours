/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTFS_FORMAT_HPP
#define BOOTFS_FORMAT_HPP 1

#include <ours/types.hpp>

#include <type_traits>

#include <cstddef>

namespace bootfs {
/// Header at the start of a BootFS blob.
///
/// Layout: Header | Entry[*] | NameTable | FileData (aligned).
struct Header final {
    static constexpr auto kMagic = u32{0x42545346}; // "BTSF", LE

    u32 magic;
    u32 file_count;
    u32 name_table_size;
    u32 reserved;
    u64 total_size;
    u64 reserved2;
};

/// Directory entry describing one file inside the BootFS blob.
struct Entry final {
    u32 name_offset; // into the name table (after the entry array)
    u16 name_size;   // strlen, must not be zero
    u16 flags;       // reserved, must be zero
    u64 data_offset; // from the start of the BootFS blob
    u64 data_size;
    u64 alignment; // power of two; data_offset must be aligned
};

static_assert(std::is_standard_layout_v<Header>);
static_assert(std::is_trivially_copyable_v<Header>);
static_assert(sizeof(Header) == 32);
static_assert(offsetof(Header, magic) == 0);
static_assert(offsetof(Header, file_count) == 4);
static_assert(offsetof(Header, name_table_size) == 8);
static_assert(offsetof(Header, reserved) == 12);
static_assert(offsetof(Header, total_size) == 16);
static_assert(offsetof(Header, reserved2) == 24);

static_assert(std::is_standard_layout_v<Entry>);
static_assert(std::is_trivially_copyable_v<Entry>);
static_assert(sizeof(Entry) == 32);
static_assert(offsetof(Entry, name_offset) == 0);
static_assert(offsetof(Entry, name_size) == 4);
static_assert(offsetof(Entry, flags) == 6);
static_assert(offsetof(Entry, data_offset) == 8);
static_assert(offsetof(Entry, data_size) == 16);
static_assert(offsetof(Entry, alignment) == 24);
} // namespace bootfs

#endif // BOOTFS_FORMAT_HPP
