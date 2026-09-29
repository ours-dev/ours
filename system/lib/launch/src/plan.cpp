/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/plan.hpp>

namespace launch {
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
auto plan_mappings(std::span<Segment const> segments, Relro relro, u64 file_size, Plan &out)
    -> ours::Status {
    // Yours to write; the specification is the comment above, and
    // test/plan_test.cpp is the check.
    return ours::Status::Unimplemented;
}
} // namespace launch
