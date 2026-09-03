/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_ZONE_HPP
#define OURS_MM_PM_ZONE_HPP 1

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/mm/priv/frame_buddy.hpp>
#include <ours/mm/priv/memory_allocation_context.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/collections/static_vec.hpp>
#include <ustl/sync/atomic.hpp>

#include <array>
#include <atomic>

namespace ours::mm {
template <usize MaxOrder>
class FramePcpuCache {
    std::array<PmFrame::List, MaxOrder> lists_;
};

/// Locked wrapper around the pure FrameBuddy. The buddy algorithm
/// itself is kept free of kernel globals so the exact same code runs in host unit
/// tests without booting a kernel.
class FrameSet {
    using Self = FrameSet;

  public:
    FrameSet() = default;

    FrameSet(PmFrame *frames, usize count, Pfn base_pfn) : m_buddy(frames, count, base_pfn) {}

    auto attach(PmFrame *frames, usize count, Pfn base_pfn) -> void {
        m_buddy.attach(frames, count, base_pfn);
    }

    /// Unlocked, both of them. The zone is the only thing that reaches a
    /// FrameSet, and it has a lock of its own; a second one here would serialise
    /// the same accesses twice and -- worse -- give the allocator two places to
    /// take a lock from, which is how a path that allocates and frees under one
    /// call ends up deadlocking against itself.
    ///
    /// So the rule is: PmZone serialises, FrameSet does not. Anything calling
    /// these holds the zone's lock.
    auto acquire_frame(usize order) -> PmFrame * {
        return m_buddy.allocate(order);
    }

    auto release_frame(PmFrame *frame, usize order) -> void {
        m_buddy.release(frame, order);
    }

  private:
    FrameBuddy m_buddy;
};

/// `PmZone` describes a memory zone with a specific responsibility and
/// serves as the core component of the page frame allocator.
///
/// TODO(SmallHuaZi) Seprate the free list from `PmZone` and refactor it
/// to `PmFrame::Lists`
class PmZone {
    using Self = PmZone;

  public:
    /// FIXME(SmallHuaZi) The cache per cpu has not been finished
    constexpr static usize MAX_PCPU_CACHE_ORDER = 0;

    enum class WaterMark {
        Critical,   // 0 - %10
        Moderate,   // %10 - %30
        Sufficient, // %30 - %100
        Count,
    };

    PmZone(NnId, ZoneType, Pfn start_pfn, Pfn end_pfn, usize present_frame);

    ~PmZone();

    auto contains(usize order) -> bool;

    /// Allocate a frame whose order equals to |order|
    ///
    /// Assumptions:
    ///     1) |order| must be lesser than `MAX_FRAME_ORDER`.
    auto alloc_frames(Gaf, usize, AllocationControl, MemoryAllocationContext const &) -> PmFrame *;

    auto alloc_frame(Gaf gaf, usize order) -> PmFrame *;

    auto free_frame(PmFrame *frame, usize order) -> void;

    auto find_frame(usize order) -> PmFrame *;

    FORCE_INLINE constexpr auto which_node() const -> NnId {
        return m_nid;
    }

    FORCE_INLINE constexpr auto zone_type() const -> ZoneType {
        return m_type;
    }

    FORCE_INLINE constexpr auto present_frames() const -> usize {
        return m_present_frames;
    }

    FORCE_INLINE constexpr auto managed_frames() const -> usize {
        return m_managed_frames;
    }

    FORCE_INLINE constexpr auto spanned_frames() const -> usize {
        return m_spanned_frames;
    }

    FORCE_INLINE constexpr auto reserved_frames() const -> usize {
        return m_reserved_frames;
    }

    FORCE_INLINE constexpr auto start_pfn() const -> usize {
        return m_start_pfn;
    }

    FORCE_INLINE constexpr auto end_pfn() const -> usize {
        return m_start_pfn + m_spanned_frames;
    }

    FORCE_INLINE constexpr auto is_below_high_wmark() const -> bool {
        return !!(m_flags & Flags::BelowHighWmark);
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(PmZone);

    enum class Flags : u32 {
        Initialized = BIT(0),
        Contiguous = BIT(1),
        BelowHighWmark = BIT(2),
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    friend class PmNode;

    FORCE_INLINE static auto is_order_within_pcpu_cache_limit(usize order) -> bool {
        return order < MAX_PCPU_CACHE_ORDER;
    }

    auto finish_allocation(PmFrame *frame, Gaf gaf, usize order) -> void;

    /// Bind the zone's memmap frame array to the buddy allocator. The
    /// memmap is populated after zones are constructed, so this runs
    /// lazily on first alloc/free.
    auto ensure_fset_attached() -> void;

    gktl::Canary m_canary;
    char const *m_name;
    PmNode *m_node;
    NnId m_nid;
    Flags m_flags;

    sync::SpinLock m_lock;
    ZoneType m_type;

    Pfn m_start_pfn;

    //! `present_pages` is physical pages existing within the zone,
    //! which is calculated as:
    //!	    present_frames_ = spanned_frames_ - absent_frames_(pages in holes).
    std::atomic<usize> m_present_frames;

    //! @brief present pages managed by the buddy system, which
    //! is calculated as (reserved_pages includes pages
    //! allocated by the booting time allocator):
    //!     managed_frames_ = present_frames_ - reserved_frames_.
    std::atomic<usize> m_managed_frames;

    std::atomic<usize> m_spanned_frames;

    std::atomic<usize> m_reserved_frames;

    std::array<usize, static_cast<usize>(WaterMark::Count)> m_watermarks;

    std::array<PmFrame::List, NR_FRAME_ORDERS> m_frames;

    typedef FramePcpuCache<MAX_PCPU_CACHE_ORDER> PcpuCache;
    CpuLocal::PerCpu<PcpuCache> m_frame_cache;

    FrameSet m_fset;
    bool m_fset_attached = false;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_ZONE_HPP