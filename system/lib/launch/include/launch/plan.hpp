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

/// Works out how to map the PT_LOAD segments of a program: which pages come
/// straight from its file, which are copied into VMOs of their own, and with
/// what protection each is mapped.
///
/// This is the core of the launcher and it does no I/O: it only decides. The
/// launcher then does what the Plan says -- vmo_create and vmo_write for every
/// private VMO, the relocations written into them, then vma_map for every
/// mapping -- and, because relocation is done before anything is mapped, each
/// mapping is made with its final protection and nothing has to be protected
/// afterwards (interface design 3.2, step 5).
///
/// |segments| are the PT_LOAD entries in the order the file has them, at most
/// Plan::MAX_SEGMENTS. |relro| is PT_GNU_RELRO. |file_size| is how many bytes
/// the program's file has.
///
/// Errors. On any of them |out| is left empty (mapping_count and copy_count
/// zero, private_size all zero), whatever it held before.
///
///   OutOfRange  more than Plan::MAX_SEGMENTS segments.
///   InvArgs     anything else the input cannot be, each of:
///     - no segments at all;
///     - a segment with mem_size 0, or file_size larger than mem_size;
///     - a segment whose flags name none of PF_R, PF_W, PF_X, or name both PF_W
///       and PF_X;
///     - a segment whose vaddr and file_offset differ in their offset within a
///       page (they must be equal modulo PAGE_SIZE, or the file's pages cannot
///       be the segment's pages);
///     - a segment whose bytes run past the end of the file, that is
///       file_offset + file_size > |file_size|;
///     - arithmetic that overflows u64: a vaddr plus mem_size, that rounded up
///       to a page, or a file_offset plus file_size;
///     - a segment that is not writable but has mem_size != file_size: pages
///       taken from the file cannot be zero-filled;
///     - segments out of order, or two sharing a page: each segment's first
///       page must be at or after the end of the last page of the one before;
///     - a RELRO that is not a prefix of a writable segment, that is one whose
///       vaddr is not that segment's vaddr, or whose size is more than its
///       mem_size, or that fits in no writable segment.
///
/// The Plan, for each segment in turn. Let `first` be its vaddr rounded down
/// to a page and `end` its vaddr plus mem_size rounded up to a page.
///
///   Not writable (PF_W not set). One mapping, Source::File: vaddr `first`,
///   size `end - first`, vmo_offset the file_offset rounded down to a page,
///   Perm::RX if PF_X is set and Perm::R if not. No private VMO.
///
///   Writable (PF_W set). A private VMO of `end - first` bytes
///   (private_size[i]). If file_size is not 0, one Copy: vmo_offset
///   `vaddr - first`, file_offset and size the segment's. The mappings are of
///   the private VMO, Source::Private, vmo_offset `page - first` for each
///   `page` that starts one, and cover [first, end) once each:
///     - with no RELRO in this segment, one, Perm::RW;
///     - with RELRO {vaddr, size}: the pages that are wholly RELRO are Perm::R
///       -- from `first` up to `vaddr + size` rounded down to a page -- and
///       the rest are Perm::RW. A page that holds RELRO and something else
///       cannot be read-only, so it stays RW, and if no page is wholly RELRO
///       there is no R mapping. Empty parts are not mapped.
///
/// A segment with PF_W and without PF_R is mapped RW, and one with PF_X and
/// without PF_R is mapped RX.
[[nodiscard]]
auto plan_mappings(std::span<Segment const> segments, Relro relro, u64 file_size, Plan &out)
    -> ours::Status;
} // namespace launch

#endif // #ifndef LAUNCH_PLAN_HPP
