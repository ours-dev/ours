/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LAUNCH_PLAN_HPP
#define LAUNCH_PLAN_HPP 1

#include <span>

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace launch {
/// The page size the launcher maps in. Every address and length in a Plan is a
/// multiple of it, except where a field says otherwise.
inline constexpr u64 PAGE_SIZE = 4096;

/// ELF program-header flags (p_flags), whose values the ELF ABI fixes.
inline constexpr u32 PF_X = 1;
inline constexpr u32 PF_W = 2;
inline constexpr u32 PF_R = 4;

/// One PT_LOAD, as the program's file describes it. Addresses are link-time:
/// relative to wherever the launcher places the image, not absolute.
struct Segment {
    u64 vaddr;       ///< p_vaddr: where the segment starts.
    u64 file_offset; ///< p_offset: where its bytes start in the file.
    u64 file_size;   ///< p_filesz: how many of them the file has.
    u64 mem_size;    ///< p_memsz: how long it is in memory. The bytes past
                     ///< file_size are zero (.bss).
    u32 flags;       ///< p_flags: PF_R | PF_W | PF_X.
};

/// PT_GNU_RELRO: a range that must be read-only once the image is relocated.
/// size == 0 means there is none.
struct Relro {
    u64 vaddr = 0;
    u64 size = 0;
};

/// The protection a mapping is made with. There is no RWX and no execute-only:
/// W^X is the system's rule (design D3), and a mapping cannot be executed
/// without being readable (vma_map answers InvArgs).
enum class Perm : u8 {
    R,
    RW,
    RX,
};

/// Where a mapping's pages come from.
enum class Source : u8 {
    /// The program's own file (the executable BootFS), mapped as it is. Only
    /// for segments that are not writable: the file is shared and never written.
    File,
    /// A VMO of this segment's own, made for it and filled from the file
    /// before it is mapped (Copy). For writable segments, and for the pages of
    /// one that are read-only once relocated (RELRO).
    Private,
};

/// One vma_map the launcher is to make.
struct Mapping {
    Source source;
    u32 segment;    ///< Index in the segments given. For Private: whose VMO.
    u64 vaddr;      ///< Where, page-aligned.
    u64 size;       ///< How much, page-aligned and not zero.
    u64 vmo_offset; ///< Page-aligned. File: an offset in the file. Private: an
                    ///< offset in that segment's private VMO.
    Perm perm;
};

/// Bytes to write into a segment's private VMO before it is mapped.
struct Copy {
    u32 segment;
    u64 vmo_offset;  ///< In the private VMO. Not page-aligned: the segment
                     ///< starts wherever its vaddr falls in its first page.
    u64 file_offset; ///< Where they come from in the file.
    u64 size;
};

/// What the launcher is to do to map a program: the VMOs to make, what to copy
/// into them, and the mappings, in that order.
struct Plan {
    static constexpr usize MAX_SEGMENTS = 8;
    /// A segment gives at most three: the RELRO pages, and the writable pages
    /// before and after them (in this order of address, not of importance).
    static constexpr usize MAX_MAPPINGS = 3 * MAX_SEGMENTS;

    /// In ascending order of vaddr.
    Mapping mappings[MAX_MAPPINGS];
    usize mapping_count = 0;

    /// At most one per segment, in ascending order of segment.
    Copy copies[MAX_SEGMENTS];
    usize copy_count = 0;

    /// Indexed like the segments: how big the private VMO of that segment is,
    /// or 0 if it has none.
    u64 private_size[MAX_SEGMENTS] = {};
};

/// Works out how to map the PT_LOAD segments of a program into |out|: which
/// pages come straight from its file, which are copied into VMOs of their own,
/// and with what protection each is mapped. |segments| are the PT_LOAD entries
/// in file order, |relro| is PT_GNU_RELRO, |file_size| the file's length.
/// OutOfRange for more than Plan::MAX_SEGMENTS segments, InvArgs for input it
/// cannot be; on error |out| is left empty. The full specification is on the
/// definition, in src/plan.cpp.
[[nodiscard]]
auto plan_mappings(std::span<Segment const> segments, Relro relro, u64 file_size, Plan &out)
    -> ours::Status;
} // namespace launch

#endif // #ifndef LAUNCH_PLAN_HPP
