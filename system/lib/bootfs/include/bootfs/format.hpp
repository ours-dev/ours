/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTFS_FORMAT_HPP
#define BOOTFS_FORMAT_HPP 1

#include <type_traits>

#include <cstddef>

#include <ours/types.hpp>

namespace bootfs {
/// The page of the format: every file's data starts on one, the Builder pads
/// the BootFS out to one, and the page rule below is stated in them.
///
/// The page rule, which bootfs::View enforces:
///   - a file's data starts on a page;
///   - no two files put bytes on a common page, unless they are the same
///     file under two names (the same offset and the same size). A file
///     without bytes covers no page, so it shares none.
///
/// Why: a program is given its file by slicing it out of the BootFS VMO, and
/// a slice is made of whole pages (vmo_create_child, which also refuses an
/// offset off a page). A file sharing its last page with a neighbour would
/// carry the neighbour's bytes in its slice, and once that slice is made
/// executable, those bytes would be executable too.
///
/// At 8 bytes, the alignment the Builder had before 2026-10-03, no file of a
/// BootFS userboot unpacked could be sliced. The BootFS the kernel used to
/// lay out itself aligned its files to a page for the same reason.
inline constexpr auto ALIGNMENT = u64{4096};

/// Header at the start of a BootFS blob.
///
/// Layout: Header | Entry[*] | NameTable | FileData (aligned).
struct Header final {
    static constexpr auto MAGIC = u32{0x42545346}; // "BTSF", LE

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
