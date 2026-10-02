/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LAUNCH_PLAN_HPP
#define LAUNCH_PLAN_HPP 1

#include <array>
#include <span>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

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
///
/// size is the header's p_memsz, not its p_filesz. LLD 18 and later end RELRO
/// with a .relro_padding section that takes no room in the file and rounds
/// RELRO up to a page, so only p_memsz reaches the end of the last RELRO page.
/// Given p_filesz, that page would be left writable.
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
    /// The segment's snapshot: a copy-on-write child of the file over the pages
    /// of a writable segment that hold nothing but file bytes. Every process
    /// running the program shares them until it writes one, which then becomes
    /// its own copy (vmo_create_child, SnapshotOnWrite).
    Snapshot,
    /// The segment's zero-fill VMO, made for it: the page its file bytes end in,
    /// when .bss follows them there, and the pages of .bss after it. Filled
    /// from the file where a Copy says so.
    Zero,
};

/// One vma_map the launcher is to make.
struct Mapping {
    Source source;
    u32 segment;    ///< Index in the segments given. Snapshot, Zero: whose VMO.
    u64 vaddr;      ///< Where, page-aligned.
    u64 size;       ///< How much, page-aligned and not zero.
    u64 vmo_offset; ///< Page-aligned. File: an offset in the file. Snapshot,
                    ///< Zero: an offset in that segment's VMO of that kind.
    Perm perm;
};

/// Where a writable segment's snapshot starts in the file and how long it is.
/// A size of 0 means the segment has none.
struct Snapshot {
    u64 file_offset; ///< Page-aligned.
    u64 size;        ///< Page-aligned.
};

/// Bytes to write into a segment's zero-fill VMO before it is mapped: the file
/// bytes of the page that .bss starts in. Every other file byte reaches memory
/// through a snapshot, without being copied.
struct Copy {
    u32 segment;
    u64 vmo_offset;  ///< In the zero-fill VMO: 0, unless the segment starts in
                     ///< the page .bss starts in, where it is vaddr's offset in
                     ///< that page.
    u64 file_offset; ///< Where they come from in the file.
    u64 size;        ///< Less than PAGE_SIZE.
};

/// What the launcher is to do to map a program: the VMOs to make, what to copy
/// into them, and the mappings, in that order.
///
/// Worked out from the program's PT_LOAD segments by build(), which is the only
/// way to get one: a Plan either exists and is whole, or build() answered an
/// error and there is none. Its contents are read, never changed, from outside.
///
/// This is the core of the launcher and it does no I/O: it only decides. The
/// launcher then does what the Plan says -- vmo_create_child(SnapshotOnWrite)
/// of the file for every snapshot, vmo_create for every zero-fill VMO and
/// vmo_write for every Copy into it, the relocations written into those VMOs,
/// then vma_map for every mapping -- and, because relocation is done before
/// anything is mapped, each mapping is made with its final protection and
/// nothing has to be protected afterwards (interface design 3.2, step 5).
class Plan {
  public:
    static constexpr usize MAX_SEGMENTS = 8;
    /// A segment gives at most three: [first, end) cut where the source changes
    /// (from the snapshot to the zero-fill VMO) and where the protection
    /// changes (the end of RELRO).
    static constexpr usize MAX_MAPPINGS = 3 * MAX_SEGMENTS;

    /// Works out how to map |segments|, the PT_LOAD entries in file order, with
    /// |relro| (PT_GNU_RELRO) in a file of |file_size| bytes. OutOfRange for
    /// more than MAX_SEGMENTS segments, InvArgs for input it cannot be. The
    /// full specification is on make(), in src/plan.cpp.
    [[nodiscard]]
    static auto build(std::span<Segment const> segments, Relro relro, u64 file_size)
        -> ustl::Result<Plan, ours::Status>;

    /// In ascending order of vaddr.
    [[nodiscard]]
    auto mappings() const -> std::span<Mapping const> {
        return {m_mappings.data(), m_mapping_count};
    }

    /// At most one per segment, in ascending order of segment.
    [[nodiscard]]
    auto copies() const -> std::span<Copy const> {
        return {m_copies.data(), m_copy_count};
    }

    /// The snapshot of segment |segment|; its size is 0 if it has none. The
    /// zero-fill VMO starts where it ends, at `vaddr` rounded down to a page plus
    /// its size: a relocation that straddles that address is written in two
    /// parts, one into each VMO.
    [[nodiscard]]
    auto snapshot(u32 segment) const -> Snapshot {
        if (segment >= MAX_SEGMENTS) {
            return {};
        }
        return m_snapshots[segment];
    }

    /// How big the zero-fill VMO of segment |segment| is, or 0 if it has none.
    [[nodiscard]]
    auto zero_size(u32 segment) const -> u64 {
        if (segment >= MAX_SEGMENTS) {
            return 0;
        }
        return m_zero_sizes[segment];
    }

  private:
    Plan() = default;

    /// Fills this Plan from the segments; build() calls it on a fresh one and
    /// throws the Plan away if it answers an error, so it may stop part way.
    [[nodiscard]]
    auto make(std::span<Segment const> segments, Relro relro, u64 file_size) -> ours::Status;

    /// The only writers of the storage below, so that the count never passes
    /// its capacity whatever make() does.
    auto add_mapping(Mapping const &mapping) -> void;
    auto add_copy(Copy const &copy) -> void;
    auto set_snapshot(u32 segment, Snapshot const &snapshot) -> void;
    auto set_zero_size(u32 segment, u64 size) -> void;

    /// Maps [from, to) of writable segment |segment| as one mapping; an empty
    /// range maps nothing. Both are page-aligned and inside the segment, and
    /// neither |zero_from| nor |relro_end| lies strictly between them: the
    /// mapping is the snapshot's if |from| is below |zero_from| and the
    /// zero-fill VMO's if not, read-only if |from| is below |relro_end|.
    auto add_writable_mapping(u32 segment, u64 first, u64 zero_from, u64 relro_end, u64 from,
                              u64 to) -> void;

    std::array<Mapping, MAX_MAPPINGS> m_mappings = {};
    usize m_mapping_count = 0;
    std::array<Copy, MAX_SEGMENTS> m_copies = {};
    usize m_copy_count = 0;
    std::array<Snapshot, MAX_SEGMENTS> m_snapshots = {};
    std::array<u64, MAX_SEGMENTS> m_zero_sizes = {};
};
} // namespace launch

#endif // #ifndef LAUNCH_PLAN_HPP
