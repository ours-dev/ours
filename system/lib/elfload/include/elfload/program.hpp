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
/// of it being separate from the loader (kelfload::Loader): a Loader belongs
/// to an address space, so by the time it can say "this is not a program" a
/// process already exists and has to be torn down again. Linux draws the
/// same line in load_elf_binary(), whose header checks all come before
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

/// One mapping of an image: whole pages of one PT_LOAD, and the protection
/// they are mapped with.
///
/// A segment is one mapping, except a writable one whose start PT_GNU_RELRO
/// covers: its pages wholly inside RELRO are a read-only mapping of their
/// own, and the rest a read-write one. Nothing but the loader writes those
/// pages, and it relocates through the image's VMO rather than through a
/// mapping, so they can be read-only from the start and nothing has to
/// protect them afterwards.
///
/// Every mapping is readable, so its protection is |write| or |execute| or
/// neither, never both. A segment whose p_flags lack PF_R is mapped readable
/// all the same, as the launcher maps it: x86-64 has no page that can be
/// written or executed without being read, and vma_map refuses to be asked
/// for one.
struct MappingPages {
    /// From the image's first page, the one that holds its lowest PT_LOAD
    /// address: where the mapping's first page is in a VMO that lays the
    /// image out from there, and in the range it is mapped into.
    u64 offset = 0;
    /// Whole pages, at least one.
    u64 size = 0;
    bool write = false;
    bool execute = false;
    /// The pages of a writable segment that are wholly RELRO, mapped
    /// read-only. Kept because the protection no longer says that the
    /// segment they come from is writable, and a caller may need to know.
    bool relro = false;
};

/// The most PT_LOAD segments an image may have here: well above what lld
/// produces (read-only, code, RELRO and data make four).
inline constexpr usize MAX_LOAD_SEGMENTS = 12;

/// A segment is at most two mappings: its RELRO pages and the rest.
inline constexpr usize MAX_MAPPINGS = 2 * MAX_LOAD_SEGMENTS;

using MappingPagesList = ustl::collections::StaticVec<MappingPages, MAX_MAPPINGS>;

/// The mappings |file| is made of, each with its own protection: what a
/// loader maps an image by. In the order of the PT_LOAD segments they come
/// from, which is ascending, and ascending within a segment. The kernel maps
/// every image it starts this way (kelfload::Loader::map_segments()), a
/// program and the vDSO alike.
///
/// The rules are the user-space launcher's (launch::Plan::make(),
/// system/lib/launch/src/plan.cpp), so that a program is mapped alike
/// whichever of the two starts it:
///   - a segment's pages run from its p_vaddr rounded down to a page to its
///     p_vaddr + p_memsz rounded up;
///   - a segment whose p_flags name none of PF_R, PF_W and PF_X, or name both
///     PF_W and PF_X, is refused: W^X is the system's rule (design D3);
///   - segments must be in ascending order, and two may not share a page,
///     which could not have two protections;
///   - every PT_GNU_RELRO must be a prefix of a writable segment, starting at
///     its p_vaddr and no longer than its p_memsz. RELRO's length is its own
///     p_memsz: LLD's .relro_padding, which rounds RELRO up to a page, takes
///     no room in the file, so p_filesz would leave the last page writable;
///   - the pages of that segment from its first one up to the end of RELRO
///     rounded down to a page are wholly RELRO, and are one read-only
///     mapping; the rest are one read-write mapping. A page that holds RELRO
///     and anything else also holds data that is written, so it stays
///     read-write, and if no page is wholly RELRO the segment is one
///     read-write mapping.
///
/// Planned by eftl's load policy at page granularity, so beside those it
/// refuses what check_program() does: not an ELF for this machine,
/// overlapping segments, more than MAX_LOAD_SEGMENTS. InvArgs for each, with
/// why through |diagnostics|, and |out| is then empty. Unlike
/// check_program(), it asks nothing of the entry point beyond its lying in
/// some segment, so it serves the vDSO as well as a program.
[[nodiscard]]
auto mapping_pages(Diagnostics const &diagnostics, ustl::views::Span<u8 const> file,
                   MappingPagesList &out) -> ours::Status;
} // namespace elfload

#endif // #ifndef ELFLOAD_PROGRAM_HPP
