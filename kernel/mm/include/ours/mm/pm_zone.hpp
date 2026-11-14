/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_ZONE_HPP
#define OURS_MM_PM_ZONE_HPP 1

#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/mm/priv/admission.hpp>
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

    /// Allocation and reclaim thresholds, ordered from lowest to highest.
    enum class Watermark : u32 {
        /// The admission floor intended for callers that cannot block.
        Min,

        /// The admission floor intended to preserve a reserve for nonblocking callers.
        Low,

        /// The reclaim target, separate from allocation admission.
        High,

        /// The number of thresholds stored for each zone.
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

    /// The zone's layer of mm::alloc_frames(): takes a block of 2^|order|
    /// frames out of this zone's stock under the zone lock, or none if the
    /// zone cannot serve it. It chooses nothing and prepares nothing: which
    /// zone to ask is the node's decision, and zeroing is the caller's.
    [[nodiscard]]
    auto alloc_frames(usize order) -> ustl::Option<PmFrame &>;

    /// The zone's layer of mm::free_frames(): puts a block this zone owns
    /// back into its stock under the zone lock.
    auto free_frames(PmFrame &frame, usize order) -> void;

    /// How many frames this zone could hand out right now, counted out of the
    /// buddy's free lists under the zone lock.
    ///
    /// Exact as of the moment the lock is held, and that is what it costs: it
    /// takes the lock the allocation path wants. num_free_frames() reads the
    /// counter snapshot without taking that lock.
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

    /// Snapshot of this zone's free-frame counter, read without the zone lock.
    ///
    /// Buddy transfers update the counter by the actual number of frames under
    /// the zone lock. A lock-free reader can observe the previous count while
    /// a buddy mutation is still in progress. The value can change before the
    /// caller uses it, so it gives no guarantee that an allocation will succeed.
    /// count_free_frames() instead counts the buddy lists under the zone lock.
    FORCE_INLINE
    constexpr auto num_free_frames() const -> usize {
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
    constexpr auto watermark(Watermark const mark) const -> usize {
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
    auto watermark_ok(usize const order, Watermark const mark) const -> bool {
        return mm::admits(num_free_frames(), order, watermark(mark));
    }

    /// Whether this zone may give 2^|order| frames to a request in |pass|:
    /// the one question a zone answers about itself for the candidate walk,
    /// from its own watermarks, its reserve and its free count. |can_block|
    /// and |highest| are the request's: whether it may block, and the
    /// highest zone type it allows, which decides how much of this zone is
    /// held back from it. The zone reads no allocation flags. A snapshot,
    /// like watermark_ok().
    [[nodiscard]]
    FORCE_INLINE
    auto admits(usize const order, AdmissionPass const pass, bool const can_block,
                ZoneType const highest) const -> bool {
        auto const floor =
            admission_floor(watermark(Watermark::Min), watermark(Watermark::Low), pass, can_block);
        return mm::admits(num_free_frames(), order, floor + lowmem_reserve(highest));
    }

    /// The frames of this zone held back from a request whose highest
    /// allowed zone type is |highest|: a share of what the zones above this
    /// one on the same node manage, up to |highest| (mm::lowmem_reserve()).
    /// Zero until PmNode::setup_watermarks() has run, and for a |highest| at
    /// or below this zone's own type.
    [[nodiscard]]
    FORCE_INLINE
    auto lowmem_reserve(ZoneType const highest) const -> usize {
        return m_lowmem_reserve[std::to_underlying(highest)];
    }

    /// The share of the memory above a zone that it keeps back: one frame in
    /// LOWMEM_RESERVE_RATIO. The same for every zone, 1/256 (decided with
    /// the user, 2026-11-14).
    constexpr static usize LOWMEM_RESERVE_RATIO = 256;

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

    /// Scales this zone's three watermarks out of the machine-wide reserve.
    ///
    /// |reserve_frames| is how many frames the machine holds back altogether
    /// and |managed_total| how many every zone manages between them, so this
    /// zone takes the share of the reserve that matches its share of the
    /// memory. Called once, by PmNode::setup_watermarks(), while no CPU is
    /// allocating.
    auto setup_watermarks(usize reserve_frames, usize managed_total) -> void;

    /// Works out this zone's reserve against every highest zone type a
    /// request may allow, from the managed frames of the zones above it on
    /// its node. Called once, by PmNode::setup_watermarks(), after every
    /// zone's managed count is final and while no CPU is allocating.
    auto setup_lowmem_reserve() -> void;

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

    /// Free frames. Buddy mutations and their count deltas share the zone
    /// lock. The atomic snapshot can be read without acquiring the lock.
    std::atomic<usize> m_free_frames;

    std::atomic<usize> m_spanned_frames;

    std::atomic<usize> m_reserved_frames;

    std::array<usize, std::to_underlying(Watermark::Count)> m_watermarks;

    /// By the highest zone type a request allows. Written once, by
    /// setup_lowmem_reserve(), and read without the zone lock after.
    std::array<usize, NR_ZONES_PER_NODE> m_lowmem_reserve;

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
