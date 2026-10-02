/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/plan.hpp>

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
///   Perm::RX if PF_X is set and Perm::R if not. No private VMO.
///
///   Writable (PF_W set). A private VMO of `end - first` bytes
///   (set_private_size(i, ...)). If file_size is not 0, one Copy: vmo_offset
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
        // zeros. Only a private VMO, which starts zeroed, can hold .bss.
        if (!writable && segment.mem_size != segment.file_size) {
            return Status::InvArgs;
        }

        // A page has one protection, so two segments cannot share one, and
        // `first < previous_end` also catches segments given out of order.
        auto const first = ustl::mm::align_down(segment.vaddr, PAGE_SIZE);
        if (first < previous_end) {
            return Status::InvArgs;
        }
        previous_end = ustl::mm::align_up(unrounded_end, PAGE_SIZE);

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
        auto const first = ustl::mm::align_down(segment.vaddr, PAGE_SIZE);
        auto const end = ustl::mm::align_up(segment.vaddr + segment.mem_size, PAGE_SIZE);

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
                .vmo_offset = ustl::mm::align_down(segment.file_offset, PAGE_SIZE),
                .perm = perm,
            });
            continue;
        }

        // A writable segment gets a VMO of its own, sized in whole pages from
        // `first`, because it starts zeroed: that is .bss, and the bytes of the
        // first page that are not the segment's. Its size cannot be worked out
        // from the Copy (a pure .bss has none) or from one Mapping (RELRO splits
        // the VMO into several), which is why it is recorded on its own.
        set_private_size(index, end - first);
        if (segment.file_size != 0) {
            // The segment's first byte is `vaddr - first` into its first page.
            add_copy({
                .segment = index,
                .vmo_offset = segment.vaddr - first,
                .file_offset = segment.file_offset,
                .size = segment.file_size,
            });
        }

        // The pages that are wholly RELRO are read-only from the start: the
        // launcher relocates into the VMO before mapping it, so there is nothing
        // to protect afterwards. The RELRO ends at `vaddr + size`, and a page
        // that it only partly fills also holds data that is written to, so it is
        // rounded down and that page stays writable. `writable_from` is where
        // the read-write part starts.
        auto writable_from = first;
        if (relro.size != 0 && segment.vaddr == relro.vaddr) {
            auto const relro_end = ustl::mm::align_down(segment.vaddr + relro.size, PAGE_SIZE);
            if (relro_end > first) {
                add_mapping({
                    .source = Source::Private,
                    .segment = index,
                    .vaddr = first,
                    .size = relro_end - first,
                    .vmo_offset = 0,
                    .perm = Perm::R,
                });
                writable_from = relro_end;
            }
        }
        // Empty when RELRO is the whole segment.
        if (end > writable_from) {
            add_mapping({
                .source = Source::Private,
                .segment = index,
                .vaddr = writable_from,
                .size = end - writable_from,
                .vmo_offset = writable_from - first,
                .perm = Perm::RW,
            });
        }
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

auto Plan::set_private_size(u32 const segment, u64 const size) -> void {
    if (segment < MAX_SEGMENTS) {
        m_private_size[segment] = size;
    }
}
} // namespace launch
