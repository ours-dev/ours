/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_ZONE_HPP
#define OURS_MM_PM_ZONE_HPP 1

#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/mm/priv/alloc_request.hpp>
#include <ours/mm/priv/frame_buddy.hpp>
#include <ours/node_mask.hpp>
#include <ours/sync/spin_lock.hpp>

#include <array>
#include <atomic>
#include <utility>

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/collections/static_vec.hpp>
#include <ustl/option.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/views/span.hpp>

namespace ours::mm {
template <usize MaxOrder>
class FramePcpuCache {
    std::array<PmFrame::List, MaxOrder> lists_;
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

    /// The three lines a zone's free count is measured against, lowest first.
    ///
    /// Min is the floor: only a caller that cannot block may draw a zone down
    /// to it, on the grounds that it has nowhere else to go. Low is where a
    /// caller that can block is turned away, so that it moves on to the next
    /// zone and leaves what is left here for callers that cannot. High is not
    /// an admission line at all -- above it the zone is comfortable, below it
    /// the zone is worth reclaiming, and saying so is the only thing that fact
    /// is for.
    ///
    /// They replaced Critical, Moderate and Sufficient (0-10%, 10-30%,
    /// 30-100%), which classified how much memory was left rather than naming
    /// lines an allocation is admitted against. That classification is a
    /// different job -- what gets reported outward as memory pressure -- and
    /// nothing read these three, so the names were free to take.
    enum class WaterMark : u32 {
        Min,
        Low,
        High,
        Count,
    };

    PmZone(NodeId, ZoneType, Pfn start_pfn, Pfn end_pfn, usize present_frame);

    ~PmZone();

    /// Cuts the memory of every node in |nodes| into zones, before any PmNode
    /// exists, and puts each in the table at its node and type.
    ///
    /// The types are laid end to end from the lowest usable PFN, each up to
    /// its |max_zone_pfn| entry, one per type and by type (0 for a type the
    /// platform has none of); a node's range cut to a type's bounds that is
    /// not empty is a zone. Each zone's structure is allocated from its own
    /// node's memory, which needs the node's number and not its PmNode.
    ///
    /// Called once, on the boot CPU, before the other CPUs run and before
    /// anything reads the table; nothing writes it afterwards.
    static auto create_all(ustl::views::Span<Pfn> max_zone_pfn, NodeMask const &nodes) -> void;

    /// The zone of node |nid| and type |type|, or null when the node has no
    /// memory of that type.
    [[nodiscard]]
    static auto at(NodeId const nid, ZoneType const type) -> PmZone * {
        DEBUG_ASSERT(nid < MAX_NODES && std::to_underlying(type) < NR_ZONES_PER_NODE);
        return TABLE[nid][std::to_underlying(type)];
    }

    /// Node |nid|'s row of the table, by type, with null where it has no
    /// memory of a type.
    [[nodiscard]]
    static auto row(NodeId const nid) -> ustl::views::Span<PmZone *const> {
        DEBUG_ASSERT(nid < MAX_NODES);
        return {TABLE[nid].data(), TABLE[nid].size()};
    }

    /// Calls |f| on every zone of every node, each exactly once: node by node,
    /// each node's in type order.
    ///
    /// No lock: the table is filled by create_all() before anything walks it,
    /// and no zone is ever added or destroyed after. A node's fallback order
    /// is no substitute: it lists the other nodes' zones too, and walking every
    /// node's would count each zone once per node.
    template <typename F>
        requires ustl::traits::Invocable<F, PmZone &>
    static auto for_each(F &&f) -> void {
        for (auto const &row: TABLE) {
            for (auto *const zone: row) {
                if (zone) {
                    f(*zone);
                }
            }
        }
    }

    /// Whether this zone could serve an allocation of |order| from its free
    /// lists right now. A snapshot under the zone lock: the answer may be
    /// stale by the time the caller acts on it, so it is for choosing where to
    /// try, never a promise that the allocation will succeed. It looks for a
    /// non-empty free list from |order| up.
    [[nodiscard]]
    auto contains(usize order) -> bool;

    /// A block of 2^|request.order()| frames for |request|, or none when this
    /// zone cannot serve it.
    ///
    /// An Option and not a pointer because a zone failing to serve an
    /// allocation is the ordinary case the walk is built around -- it is why
    /// there is a fallback order at all -- and the caller has to handle it on
    /// every call. Null for "nothing" is the shape that let a null travel
    /// inside `ustl::ok()` as far as the frame-list loops; `ktl::Result` stays
    /// where it belongs, at the public interface in pmm.hpp, where a caller
    /// distinguishes one failure from another. Here there is one.
    ///
    /// Assumptions:
    ///     1) |request.order()| must be lesser than `MAX_FRAME_ORDER`.
    auto alloc_frames(AllocRequest const &request) -> ustl::Option<PmFrame &>;

    auto alloc_frame(Gaf gaf, usize order) -> ustl::Option<PmFrame &>;

    auto free_frame(PmFrame *frame, usize order) -> void;

    /// How many frames this zone could hand out right now, counted out of the
    /// buddy's free lists under the zone lock.
    ///
    /// Exact as of the moment the lock is held, and that is what it costs: it
    /// takes the lock the allocation path wants. For the cheap approximate
    /// answer, which is the one anything on that path should ask, see
    /// free_frames() below.
    [[nodiscard]]
    auto count_free_frames() -> usize;

    /// How many free blocks of exactly |order| this zone holds, under the zone
    /// lock. Which orders the free memory sits at is what shows whether freed
    /// blocks coalesce; for diagnostics and tests, not a hot path.
    [[nodiscard]]
    auto free_blocks(usize order) -> usize;

    auto find_frame(usize order) -> PmFrame *;

    FORCE_INLINE
    constexpr auto which_node() const -> NodeId {
        return m_nid;
    }

    FORCE_INLINE
    constexpr auto zone_type() const -> ZoneType {
        return m_type;
    }

    FORCE_INLINE
    constexpr auto present_frames() const -> usize {
        return m_present_frames;
    }

    /// How many frames this zone can hand out, to within one allocation.
    ///
    /// A plain atomic the buddy keeps on both of its sides, so it is cheap
    /// enough for the allocation path to consult before it takes the zone
    /// lock. It is an approximation by construction: another CPU may allocate
    /// or free between the read and the act, which is why the answer is only
    /// ever used to decide where to try, never as a promise. The exact count
    /// is count_free_frames(), and it costs the lock.
    FORCE_INLINE
    constexpr auto free_frames() const -> usize {
        return m_free_frames;
    }

    /// How many frames the buddy owns in this zone: everything present except
    /// what the boot allocator took before the buddy existed.
    ///
    /// Computed rather than stored, because it is read once at setup and never
    /// on a hot path, and because a stored copy is a third counter to keep in
    /// step with the two it is derived from. It is the denominator the zone's
    /// watermarks are scaled by.
    FORCE_INLINE
    constexpr auto managed_frames() const -> usize {
        return m_present_frames - m_reserved_frames;
    }

    FORCE_INLINE
    constexpr auto spanned_frames() const -> usize {
        return m_spanned_frames;
    }

    FORCE_INLINE
    constexpr auto reserved_frames() const -> usize {
        return m_reserved_frames;
    }

    /// Records |count| frames of this zone as taken before the buddy existed.
    ///
    /// Only init_memmap() calls this, single-threaded, once per reserved range.
    /// It exists because m_reserved_frames had no writer at all: the field is
    /// documented as the difference between present and managed frames, and
    /// PmNode::dump() printed it, but every zone reported zero however much
    /// early memory it had lost.
    FORCE_INLINE
    auto add_reserved_frames(usize const count) -> void {
        m_reserved_frames += count;
    }

    /// Gives back |count| reserved frames that are reserved no more
    /// (unwire_frame()): the kernel's `.init*` sections once boot is done, and
    /// the boot image's frames once userboot has them. The subtraction is
    /// atomic, like the addition; the check before it is not, which is enough
    /// for frames that each have one owner unwiring them, and for a count only
    /// diagnostics read.
    FORCE_INLINE
    auto sub_reserved_frames(usize const count) -> void {
        DEBUG_ASSERT(count <= m_reserved_frames, "more reserved frames released than recorded");
        m_reserved_frames -= count;
    }

    FORCE_INLINE
    constexpr auto start_pfn() const -> usize {
        return m_start_pfn;
    }

    FORCE_INLINE
    constexpr auto end_pfn() const -> usize {
        return m_start_pfn + m_spanned_frames;
    }

    /// How many frames this zone's free count has to stay at or above for an
    /// allocation held to |mark| to be admitted.
    ///
    /// Zero until PmNode::setup_watermarks() has run, and zero admits
    /// everything -- which is what every zone looks like for the whole of
    /// bring-up, on purpose: there is nothing to hold memory back for before
    /// the allocator is serving anyone.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto watermark(WaterMark const mark) const -> usize {
        return m_watermarks[std::to_underlying(mark)];
    }

    /// Whether |order| frames can come out of this zone without taking its
    /// free count below |mark|.
    ///
    /// Reads the approximate free count rather than the exact one, so that a
    /// zone known to be short can be passed over without taking its lock. The
    /// answer can be stale either way the moment it is returned, which is why
    /// it may only be used to choose where to try: a yes that was wrong costs
    /// an allocation that dips a little below the line, and a no that was
    /// wrong costs a walk to the next zone.
    ///
    /// It does not ask whether a block of |order| is actually free. That needs
    /// the zone lock, and the buddy answers it by failing; contains() is the
    /// form of the question that pays for a definite answer.
    [[nodiscard]]
    FORCE_INLINE
    auto watermark_ok(usize const order, WaterMark const mark) const -> bool {
        auto const free = free_frames();
        auto const wanted = usize{1} << order;
        if (free < wanted) {
            return false;
        }
        return free - wanted >= watermark(mark);
    }

    FORCE_INLINE
    constexpr auto is_below_high_wmark() const -> bool {
        return !!(m_flags & Flags::BelowHighWmark);
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(PmZone);

    enum class Flags : u32 {
        Initialized = 0_bit,
        Contiguous = 1_bit,
        BelowHighWmark = 2_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    friend class PmNode;

    FORCE_INLINE
    static auto is_order_within_pcpu_cache_limit(usize order) -> bool {
        return order < MAX_PCPU_CACHE_ORDER;
    }

    auto finish_allocation(PmFrame *frame, Gaf gaf, usize order) -> void;

    /// Scales this zone's three watermarks out of the machine-wide reserve.
    ///
    /// |reserve_frames| is how many frames the machine holds back altogether
    /// and |managed_total| how many every zone manages between them, so this
    /// zone takes the share of the reserve that matches its share of the
    /// memory. Called once, by PmNode::setup_watermarks(), while no CPU is
    /// allocating.
    auto setup_watermarks(usize reserve_frames, usize managed_total) -> void;

    /// Every zone, at its node and type; null where a node has no memory of a
    /// type. Written by create_all() alone. A fixed grid of every node the
    /// kernel can have by every type, so that a zone is found by its two
    /// numbers, and a zone added later would only fill a cell.
    static inline constinit std::array<std::array<PmZone *, NR_ZONES_PER_NODE>, MAX_NODES> TABLE{};

    gktl::Canary m_canary;
    char const *m_name;
    PmNode *m_node;
    NodeId m_nid;
    Flags m_flags;

    sync::SpinLock m_lock;
    ZoneType m_type;

    Pfn m_start_pfn;

    //! `present_pages` is physical pages existing within the zone,
    //! which is calculated as:
    //!	    present_frames_ = spanned_frames_ - absent_frames_(pages in holes).
    std::atomic<usize> m_present_frames;

    /// Frames this zone can hand out, maintained on both sides of the buddy:
    /// down in finish_allocation(), up in free_frame().
    ///
    /// It starts at zero and grows as the handoff releases frames into the
    /// buddy, so it cannot go negative: a frame has to be freed in before it
    /// can be allocated out. It carried the name m_managed_frames and the
    /// comment of a constant total -- present minus reserved -- which it never
    /// was; that total is managed_frames(), computed from the two counters
    /// that do hold it.
    std::atomic<usize> m_free_frames;

    std::atomic<usize> m_spanned_frames;

    std::atomic<usize> m_reserved_frames;

    std::array<usize, std::to_underlying(WaterMark::Count)> m_watermarks;

    typedef FramePcpuCache<MAX_PCPU_CACHE_ORDER> PcpuCache;
    CpuLocal::PerCpu<PcpuCache> m_frame_cache;

    /// The zone's free lists.
    ///
    /// The buddy has no lock of its own, on purpose, and this zone is the only
    /// thing that reaches it: a second lock down there would serialise the
    /// same accesses twice and, worse, give the allocator two places to take a
    /// lock from, which is how a path that allocates and frees under one call
    /// ends up deadlocking against itself. So the rule is that PmZone
    /// serialises and the buddy does not, and every use of this member holds
    /// m_lock -- except the constructor's, which gives it the zone's span
    /// before the zone is on any list another CPU could reach it through.
    ///
    /// The buddy used to sit behind a FrameSet that added no behaviour of its
    /// own: five methods over this member, three of them one-line forwards,
    /// with one caller. Its two loops are now where they are used, in
    /// count_free_frames() and contains().
    FrameBuddy m_buddy TA_GUARDED(m_lock);
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_ZONE_HPP