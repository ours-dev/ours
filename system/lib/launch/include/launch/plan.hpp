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
///
/// Worked out from the program's PT_LOAD segments by build(), which is the only
/// way to get one: a Plan either exists and is whole, or build() answered an
/// error and there is none. Its contents are read, never changed, from outside.
///
/// This is the core of the launcher and it does no I/O: it only decides. The
/// launcher then does what the Plan says -- vmo_create and vmo_write for every
/// private VMO, the relocations written into them, then vma_map for every
/// mapping -- and, because relocation is done before anything is mapped, each
/// mapping is made with its final protection and nothing has to be protected
/// afterwards (interface design 3.2, step 5).
class Plan {
  public:
    static constexpr usize MAX_SEGMENTS = 8;
    /// A segment gives at most three: the RELRO pages, and the writable pages
    /// before and after them (in this order of address, not of importance).
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

    /// How big the private VMO of segment |segment| is, or 0 if it has none.
    [[nodiscard]]
    auto private_size(u32 segment) const -> u64 {
        return segment < MAX_SEGMENTS ? m_private_size[segment] : 0;
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
    auto set_private_size(u32 segment, u64 size) -> void;

    std::array<Mapping, MAX_MAPPINGS> m_mappings = {};
    usize m_mapping_count = 0;
    std::array<Copy, MAX_SEGMENTS> m_copies = {};
    usize m_copy_count = 0;
    std::array<u64, MAX_SEGMENTS> m_private_size = {};
};
} // namespace launch

#endif // #ifndef LAUNCH_PLAN_HPP
