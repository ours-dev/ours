/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_PM_ZONE_HPP
#define OURS_MEM_PM_ZONE_HPP 1

#include <ours/mem/gaf.hpp>
#include <ours/mem/pm-frame.hpp>
#include <ours/mem/cpu-local.hpp>
#include <ours/mem/priv/memory-allocation-context.hpp>

#include <ours/sync/spin-lock.hpp>
#include <ours/lockdep.hpp>

#include <ustl/sync/atomic.hpp>
#include <ustl/sync/mutex.hpp>
#include <ustl/sync/lockguard.hpp>
#include <ustl/collections/static-vec.hpp>
#include <ustl/collections/intrusive/slist.hpp>

#include <gktl/range.hpp>
#include <gktl/canary.hpp>

namespace ours::mem {
    template <usize MaxOrder>
    class FramePcpuCache {
        ustl::Array<PmFrame::List, MaxOrder> lists_;
    };

    class FrameSet {
        using Self = FrameSet;
      public:
        CXX11_CONSTEXPR
        static usize const kNumOrders = NR_FRAME_ORDERS;

        CXX11_CONSTEXPR
        static usize const kMaxFrameOrder = MAX_FRAME_ORDER;

        auto acquire_frame(usize order) -> PmFrame * {
            Guard guard{m_lock};
            return acquire_frame_locked(order);
        }

        auto release_frame(PmFrame *frame, usize order) -> void {
            Guard guard{m_lock};
            return release_frame_locked(frame, order);
        }
      private:
        auto acquire_frame_locked(usize order) -> PmFrame *;

        auto acquire_frame_inner(PmFrame *frame, Pfn pfn, usize order) -> PmFrame *;

        auto release_frame_locked(PmFrame *frame, usize order) -> void;

        auto release_frame_inner(PmFrame *frame, Pfn pfn, usize order) -> void;

        auto remove_frame(PmFrame *frame, usize order) -> void;

        auto insert_frame(PmFrame *frame, usize order) -> void;

        auto has_frame(usize order) const -> bool;

        auto get_frame(usize order) -> PmFrame *;

        sync::SpinLock m_lock;
        ustl::sync::AtomicU8 nr_lists_;
        ustl::Array<PmFrame::List, NR_FRAME_ORDERS> lists_;
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
        CXX11_CONSTEXPR
        static usize const kMaxPcpuCacheOrder = 0;

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

        FORCE_INLINE CXX11_CONSTEXPR
        auto which_node() const -> NnId { return m_nid; }

        FORCE_INLINE CXX11_CONSTEXPR
        auto zone_type() const -> ZoneType { return m_type; }

        FORCE_INLINE CXX11_CONSTEXPR
        auto present_frames() const -> usize { return m_present_frames; }

        FORCE_INLINE CXX11_CONSTEXPR
        auto managed_frames() const -> usize { return m_managed_frames; }

        FORCE_INLINE CXX11_CONSTEXPR
        auto spanned_frames() const -> usize { return m_spanned_frames; }

        FORCE_INLINE CXX11_CONSTEXPR
        auto reserved_frames() const -> usize { return m_reserved_frames; }

        FORCE_INLINE CXX11_CONSTEXPR
        auto start_pfn() const -> usize { return m_start_pfn; }

        FORCE_INLINE CXX11_CONSTEXPR
        auto end_pfn() const -> usize { return m_start_pfn + m_spanned_frames; }

        FORCE_INLINE CXX11_CONSTEXPR
        auto is_below_high_wmark() const -> bool { return !!(m_flags & Flags::BelowHighWmark); }
      private:
        USTL_NO_MOVEABLE_AND_COPYABLE(PmZone);

        enum class Flags: u32 {
            Initialized = BIT(0),
            Contiguous  = BIT(1),
            BelowHighWmark = BIT(2),
        };
        USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

        friend class PmNode;

        FORCE_INLINE
        static auto is_order_within_pcpu_cache_limit(usize order) -> bool {  
            return order < kMaxPcpuCacheOrder;
        }

        auto finish_allocation(PmFrame *frame, Gaf gaf, usize order) -> void;

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
        ustl::sync::AtomicUsize m_present_frames;

        //! @brief present pages managed by the buddy system, which
        //! is calculated as (reserved_pages includes pages 
        //! allocated by the booting time allocator):
        //!     managed_frames_ = present_frames_ - reserved_frames_.
        ustl::sync::AtomicUsize m_managed_frames;

        ustl::sync::AtomicUsize m_spanned_frames;

        ustl::sync::AtomicUsize m_reserved_frames;

        ustl::Array<usize, static_cast<usize>(WaterMark::Count)> m_watermarks;

        ustl::Array<PmFrame::List, NR_FRAME_ORDERS> m_frames;

        typedef FramePcpuCache<kMaxPcpuCacheOrder>  PcpuCache;
        CpuLocal::PerCpu<PcpuCache> m_frame_cache;

        FrameSet m_fset;
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_PM_ZONE_HPP