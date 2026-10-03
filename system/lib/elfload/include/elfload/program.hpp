/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ELFLOAD_PROGRAM_HPP
#define ELFLOAD_PROGRAM_HPP 1

#include <elfload/diagnostics.hpp>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/collections/static_vec.hpp>
#include <ustl/views/span.hpp>

namespace elfload {
/// Whether |file| is a program this kernel can start: an ELF for this
/// machine, of type ET_EXEC or ET_DYN, whose PT_LOAD segments do not
/// overlap, and whose entry point lies inside one of them that is
/// executable.
///
/// Asked before anything is built for the program, which is the whole point
/// of it being separate from Loader::map(): a Loader belongs to an address
/// space, so by the time it can say "this is not a program" a process
/// already exists and has to be torn down again. Linux draws the same line
/// in load_elf_binary(), whose header checks all come before
/// begin_new_exec(), the point after which the old image is gone.
///
/// Not asked of the vDSO, whose e_entry means nothing -- nothing jumps to a
/// vDSO -- and which this would rightly refuse.
///
/// Why it refused goes to |diagnostics|, as an Error.
[[nodiscard]]
auto check_program(Diagnostics const &diagnostics, ustl::views::Span<u8 const> file)
    -> ours::Status;

/// The granule a mapping is made of, the x86-64 page: a segment's protection
/// can only be given to whole pages of it.
inline constexpr u64 MAPPING_PAGE_SIZE = 4096;

/// One PT_LOAD of an image, as the whole pages a mapping of it with its own
/// protection takes.
struct SegmentPages {
    /// From the image's first page, the one that holds its lowest PT_LOAD
    /// address: where the segment's first page is in a VMO that lays the image
    /// out from there, and in the range it is mapped into.
    u64 offset = 0;
    /// Whole pages, at least one.
    u64 size = 0;
    bool read = false;
    bool write = false;
    bool execute = false;
};

/// The most PT_LOAD segments an image may have here: well above what lld
/// produces (read-only, code, RELRO and data make four).
inline constexpr usize MAX_LOAD_SEGMENTS = 12;

using SegmentPagesList = ustl::collections::StaticVec<SegmentPages, MAX_LOAD_SEGMENTS>;

/// |file|'s PT_LOAD segments, in program-header order, each as the pages a
/// mapping of it with its own protection takes: what a loader needs to map
/// an image by segment rather than as one range (docs/repairs/260919/
/// elfload.md, item 1).
///
/// Planned by eftl's load policy at page granularity, so beside what
/// check_program() refuses -- not an ELF for this machine, overlapping
/// segments, more than MAX_LOAD_SEGMENTS -- it refuses two segments that
/// share a page, which could not be given different protections. InvArgs,
/// with why through |diagnostics|. Unlike check_program(), it asks nothing of
/// the entry point beyond its lying in some segment, so it serves the vDSO
/// as well as a program.
[[nodiscard]]
auto segment_pages(Diagnostics const &diagnostics, ustl::views::Span<u8 const> file,
                   SegmentPagesList &out) -> ours::Status;
} // namespace elfload

#endif // #ifndef ELFLOAD_PROGRAM_HPP
