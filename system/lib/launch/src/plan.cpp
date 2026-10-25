/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/plan.hpp>

#include <algorithm>
#include <limits>

#include <ustl/mem/align.hpp>

namespace launch {
namespace {
    /// True if `a + b` does not fit in a u64. The test is written as a comparison
    /// against what is left, never as `a + b < a`, so that it is itself free of the
    /// overflow it looks for.
    constexpr auto add_overflows(u64 const a, u64 const b) -> bool {
        return a > std::numeric_limits<u64>::max() - b;
    }
} // namespace

/// Works out how to map the PT_LOAD segments of a program into this Plan: which pages come
/// straight from its file, which from a copy-on-write snapshot of it, which
/// from a zero-filled VMO, and with what protection each is mapped.
///
/// This is the core of the launcher and it does no I/O: it only decides. The
/// launcher then does what the Plan says -- vmo_create_child(SnapshotOnWrite)
/// of the file for every snapshot, vmo_create for every zero-fill VMO and
/// vmo_write for every Copy into it, the relocations written into those VMOs,
/// then vma_map for every mapping -- and, because relocation is done before
/// anything is mapped, each mapping is made with its final protection and
/// nothing has to be protected afterwards (interface design 3.2, step 5).
///
/// |segments| are the PT_LOAD entries in the order the file has them, at most
/// Plan::MAX_SEGMENTS. |relro| is PT_GNU_RELRO. |file_size| is how many bytes
/// the program's file has.
///
/// Errors. On any of them build() returns the error and no Plan, so make() may
/// stop wherever it finds one.
///
///   OutOfRange  more than MAX_SEGMENTS segments.
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
///   Perm::RX if PF_X is set and Perm::R if not. No snapshot and no zero-fill
///   VMO.
///
///   Writable (PF_W set). The pages [first, end) are split at `zero_from`:
///     - `zero_from` is `end` when mem_size == file_size (no .bss), and
///       otherwise `vaddr + file_size` rounded down to a page: the page the
///       file bytes end in, which .bss shares with them, or where .bss starts
///       when they end on a page boundary.
///     - [first, zero_from), if not empty, is the snapshot: Snapshot{
///       file_offset rounded down to a page, zero_from - first}. These pages
///       hold file bytes only (the bytes before vaddr in the first page are
///       whatever the file has there, as the file's own pages would show), so
///       they need no copy: they are shared until written.
///     - [zero_from, end), if not empty, is the zero-fill VMO, of
///       `end - zero_from` bytes. If file bytes reach into it -- the segment's
///       data does not end on a page boundary -- one Copy writes them in:
///       from `max(vaddr, zero_from)` to `vaddr + file_size`, at vmo_offset
///       `max(vaddr, zero_from) - zero_from`, from the matching file_offset.
///       A pure .bss (file_size 0) has no snapshot and no Copy.
///   The mappings cover [first, end) once each, cut at `zero_from` (Snapshot
///   below it, vmo_offset `page - first`; Zero at or above it, vmo_offset
///   `page - zero_from`) and at the end of RELRO:
///     - with no RELRO in this segment, everything is Perm::RW;
///     - with RELRO {vaddr, size}: the pages that are wholly RELRO are Perm::R
///       -- from `first` up to `vaddr + size` rounded down to a page -- and
///       the rest are Perm::RW. A page that holds RELRO and something else
///       cannot be read-only, so it stays RW, and if no page is wholly RELRO
///       there is no R mapping.
///   Empty parts are not mapped, so a writable segment gives one to three
///   mappings, in ascending order of vaddr.
///
/// The cut at `zero_from` puts the file pages first, then the intersecting
/// page and .bss as zero-fill memory with the partial page's file bytes
/// copied in. The bytes after the file data in that page cannot come from
/// the file, which has whatever follows the segment there, and they must
/// read as zero. When the segment starts in that page, the Copy starts at
/// vaddr, so the bytes before vaddr read as zero rather than as the file's:
/// they are not the segment's, and ELF promises nothing about them.
///
/// |relro| is PT_GNU_RELRO with size its p_memsz (see Relro).
///
/// A segment with PF_W and without PF_R is mapped RW, and one with PF_X and
/// without PF_R is mapped RX.
auto Plan::make(std::span<Segment const> segments, Relro const relro, u64 const file_size)
    -> ours::Status {
    using ours::Status;

    if (segments.empty()) {
        return Status::InvArgs;
    }
    if (segments.size() > MAX_SEGMENTS) {
        return Status::OutOfRange;
    }

    // Pass 1 only judges. Everything the second pass does with a segment --
    // `vaddr + mem_size`, the rounding up, `end - first` -- is arithmetic that
    // would wrap for a hostile file, and the file is not trustworthy. Proving
    // once, here, that none of it can wrap lets pass 2 add and subtract
    // without a check at every line. It also means a refused program leaves no
    // half-made mappings to reason about, though build() would throw them away
    // anyway.

    // End of the last page of the segment before; the next segment's first page
    // must not be below it. Starts at 0 so the first segment is unconstrained.
    u64 previous_end = 0;
    // A RELRO of size 0 is "none", which is trivially placed. Otherwise it is
    // placed once a writable segment that it is a prefix of has been found.
    auto relro_placed = relro.size == 0;

    for (auto const &segment: segments) {
        auto const writable = (segment.flags & PF_W) != 0;
        auto const executable = (segment.flags & PF_X) != 0;
        auto const access = segment.flags & (PF_R | PF_W | PF_X);

        if (segment.mem_size == 0 || segment.file_size > segment.mem_size) {
            return Status::InvArgs;
        }
        // W^X is the system's rule (design D3). Bits that are not access bits
        // are ignored on purpose: they carry nothing the launcher acts on.
        if (access == 0 || (writable && executable)) {
            return Status::InvArgs;
        }
        // The page of the file at file_offset is what will sit at vaddr, so the
        // two must fall at the same place in their pages, or the mapping would
        // show the wrong bytes at the right address.
        if (segment.vaddr % PAGE_SIZE != segment.file_offset % PAGE_SIZE) {
            return Status::InvArgs;
        }
        // The bytes must be inside the file. Written as offset-then-remaining
        // instead of `file_offset + file_size > file_size` because that sum can
        // wrap and then wrongly pass.
        if (segment.file_offset > file_size ||
            segment.file_size > file_size - segment.file_offset) {
            return Status::InvArgs;
        }
        // `vaddr + mem_size` is where the segment ends; rounding that up to a
        // page adds up to PAGE_SIZE - 1 on top, which is a second place to wrap.
        if (add_overflows(segment.vaddr, segment.mem_size)) {
            return Status::InvArgs;
        }
        auto const unrounded_end = segment.vaddr + segment.mem_size;
        if (add_overflows(unrounded_end, PAGE_SIZE - 1)) {
            return Status::InvArgs;
        }
        // Pages taken straight from the file cannot be zero-filled: the bytes of
        // the last page past file_size are whatever follows in the file, not
        // zeros. Only a zero-fill VMO can hold .bss, and a segment that is not
        // writable has none.
        if (!writable && segment.mem_size != segment.file_size) {
            return Status::InvArgs;
        }

        // A page has one protection, so two segments cannot share one, and
        // `first < previous_end` also catches segments given out of order.
        auto const first = ustl::round_down(segment.vaddr, PAGE_SIZE);
        if (first < previous_end) {
            return Status::InvArgs;
        }
        previous_end = ustl::round_up(unrounded_end, PAGE_SIZE);

        // RELRO is what a linker puts at the start of a writable segment, so it
        // belongs to the writable segment that starts where it does. Its size is
        // compared with mem_size rather than added to a vaddr, which is why it
        // needs no overflow check of its own.
        if (writable && relro.size != 0 && segment.vaddr == relro.vaddr) {
            if (relro.size > segment.mem_size) {
                return Status::InvArgs;
            }
            relro_placed = true;
        }
    }
    // Covers RELRO that starts no segment, that starts a segment that is not
    // writable, and that lies in no segment at all.
    if (!relro_placed) {
        return Status::InvArgs;
    }

    // Pass 2 writes the Plan. Everything is known to be in range now.
    for (usize i = 0; i < segments.size(); ++i) {
        auto const &segment = segments[i];
        auto const index = static_cast<u32>(i);
        auto const first = ustl::round_down(segment.vaddr, PAGE_SIZE);
        auto const end = ustl::round_up(segment.vaddr + segment.mem_size, PAGE_SIZE);

        if ((segment.flags & PF_W) == 0) {
            // The file is shared and never written, so it is mapped as it is. The
            // file offset rounds down, like vaddr does, so that both name the start
            // of the same page.
            auto perm = Perm::R;
            if ((segment.flags & PF_X) != 0) {
                perm = Perm::RX;
            }
            add_mapping({
                .source = Source::File,
                .segment = index,
                .vaddr = first,
                .size = end - first,
                .vmo_offset = ustl::round_down(segment.file_offset, PAGE_SIZE),
                .perm = perm,
            });
            continue;
        }

        // Where the file bytes stop being the whole of a page. With no .bss
        // the last page's bytes past file_size are past mem_size too, and
        // nothing promises what they hold, so every page can come from the file.
        // With .bss they must read as zero, and the file has whatever follows
        // the segment there, so that page and everything after it are zero-fill.
        auto const data_end = segment.vaddr + segment.file_size;
        auto zero_from = end;
        if (segment.mem_size != segment.file_size) {
            zero_from = ustl::round_down(data_end, PAGE_SIZE);
        }

        // The pages before it are file bytes only, and are shared until written.
        // vaddr and file_offset fall at the same place in their pages (checked
        // in pass 1), so `first` is the page of the file at file_offset rounded
        // down.
        if (zero_from > first) {
            set_snapshot(index, {
                                    .file_offset = ustl::round_down(segment.file_offset, PAGE_SIZE),
                                    .size = zero_from - first,
                                });
        }
        if (end > zero_from) {
            set_zero_size(index, end - zero_from);
            // The file bytes that share their page with .bss. `copy_from` is
            // vaddr itself when the segment also starts in that page.
            auto const copy_from = std::max(segment.vaddr, zero_from);
            if (data_end > copy_from) {
                add_copy({
                    .segment = index,
                    .vmo_offset = copy_from - zero_from,
                    .file_offset = segment.file_offset + (copy_from - segment.vaddr),
                    .size = data_end - copy_from,
                });
            }
        }

        // The pages that are wholly RELRO are read-only from the start: the
        // launcher relocates into the VMOs before mapping them, so there is
        // nothing to protect afterwards. The RELRO ends at `vaddr + size`, and a
        // page that it only partly fills also holds data that is written to, so
        // it is rounded down and that page stays writable. It is at most
        // mem_size long (pass 1), so `relro_end` is never past `end`.
        auto relro_end = first;
        if (relro.size != 0 && segment.vaddr == relro.vaddr) {
            relro_end = std::max(first, ustl::round_down(segment.vaddr + relro.size, PAGE_SIZE));
        }

        // [first, end) cut at the two places where the source or the protection
        // changes; a part between two cuts has one of each.
        auto const lower_cut = std::min(zero_from, relro_end);
        auto const upper_cut = std::max(zero_from, relro_end);
        add_writable_mapping(index, first, zero_from, relro_end, first, lower_cut);
        add_writable_mapping(index, first, zero_from, relro_end, lower_cut, upper_cut);
        add_writable_mapping(index, first, zero_from, relro_end, upper_cut, end);
    }

    return Status::Ok;
}

auto Plan::build(std::span<Segment const> segments, Relro const relro, u64 const file_size)
    -> ustl::Result<Plan, ours::Status> {
    Plan plan;
    if (auto const status = plan.make(segments, relro, file_size); status != ours::Status::Ok) {
        return ustl::err(status);
    }
    return ustl::ok(plan);
}

/// Past capacity is a bug in make(), not something the input can cause: the
/// capacities follow from MAX_SEGMENTS, which make() checks first. So it is
/// dropped rather than written past the array; a test that counts mappings
/// catches it.
auto Plan::add_mapping(Mapping const &mapping) -> void {
    if (m_mapping_count < MAX_MAPPINGS) {
        m_mappings[m_mapping_count] = mapping;
        m_mapping_count += 1;
    }
}

auto Plan::add_copy(Copy const &copy) -> void {
    if (m_copy_count < MAX_SEGMENTS) {
        m_copies[m_copy_count] = copy;
        m_copy_count += 1;
    }
}

auto Plan::set_snapshot(u32 const segment, Snapshot const &snapshot) -> void {
    if (segment < MAX_SEGMENTS) {
        m_snapshots[segment] = snapshot;
    }
}

auto Plan::set_zero_size(u32 const segment, u64 const size) -> void {
    if (segment < MAX_SEGMENTS) {
        m_zero_sizes[segment] = size;
    }
}

/// An empty part is not mapped. Whether a part is the snapshot's or the
/// zero-fill VMO's, and whether it is read-only, is the same for all of it,
/// because the caller cuts at both places where either changes.
auto Plan::add_writable_mapping(u32 const segment, u64 const first, u64 const zero_from,
                                u64 const relro_end, u64 const from, u64 const to) -> void {
    if (to <= from) {
        return;
    }
    auto source = Source::Snapshot;
    auto vmo_offset = from - first;
    if (from >= zero_from) {
        source = Source::Zero;
        vmo_offset = from - zero_from;
    }
    auto perm = Perm::RW;
    if (from < relro_end) {
        perm = Perm::R;
    }
    add_mapping({
        .source = source,
        .segment = segment,
        .vaddr = from,
        .size = to - from,
        .vmo_offset = vmo_offset,
        .perm = perm,
    });
}
} // namespace launch
