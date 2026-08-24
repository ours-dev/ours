/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_NODE_HPP
#define OURS_MM_PM_NODE_HPP 1

#include <gktl/canary.hpp>
#include <kmrd/damon.hpp>
#include <ktl/result.hpp>
#include <ours/assert.hpp>
#include <ours/cpu-mask.hpp>
#include <ours/init.hpp>
#include <ours/macro_abi.hpp>
#include <ours/mm/cpu-local.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/node-mask.hpp>
#include <ours/mm/page-queues.hpp>
#include <ours/mm/priv/memory-allocation-context.hpp>
#include <ours/mm/priv/memory-allocation-policy.hpp>
#include <ours/sync/spin-lock.hpp>
#include <ustl/array.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/collections/static-vec.hpp>
#include <ustl/collections/vec.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/function/fn.hpp>
#include <ustl/function/invoke.hpp>
#include <ustl/iterator/traits.hpp>
#include <ustl/mem/address_of.hpp>
#include <ustl/result.hpp>
#include <ustl/sync/atomic.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/traits/is_same.hpp>
#include <ustl/util/pair.hpp>
#include <ustl/views/span.hpp>

namespace ours::mm {
struct NodeRoute {
    NodeRoute() : count_(), path_() {}

    NodeRoute(NodeRoute &&) = default;
    NodeRoute(NodeRoute const &) = default;

    ~NodeRoute() = default;

    template <typename F, typename... Args>
    auto for_each_node(F &&f, Args &&...args) -> ustl::traits::InvokeResultT<F, PmNode &, Args...>;

    FORCE_INLINE constexpr auto set_next(NnId to) -> void {
        count_ += 1;
        path_.push_back(to);
    }

    FORCE_INLINE constexpr auto clear() -> void {
        path_.clear();
    }

    FORCE_INLINE constexpr auto begin() {
        return path_.begin();
    }

    FORCE_INLINE constexpr auto end() {
        return begin() + count_;
    }

    usize count_;
    ustl::collections::StaticVec<NnId, NUMA_MAX_NODE - 1> path_;
};

/// `ZoneQueues` is a helper class separated from `PmNode`, specifically
/// designed to manage zones of different priorities within a node and provide
/// cross-node zone access and communication capabilities.
class ZoneQueues {
    using Self = ZoneQueues;
    using ZoneRef = PmZone *;
    using ZoneQueue = ustl::views::Span<ZoneRef>;

  public:
    enum QueueType {
        /// A zone queue which arranged by zone type, there may be holes if a type of zone do not exist
        /// in the node. It's just useful when we free a frame.
        LocalSequential,
        LocalContiguous,
        SharedAffinity,
        SharedSequential,
        MaxNumQueues,
    };

    ZoneQueues(NnId nid) : nid_(nid), queues_(), queue_storage_(), connected_(), queue_sizes_() {
        init_queues();
    }

    ~ZoneQueues() = default;

    FORCE_INLINE constexpr auto insert_local_zone(PmZone *zone, ZoneType ztype) -> void {
        DEBUG_ASSERT(ZoneValue(ztype) < NR_ZONES_PER_NODE);
        DEBUG_ASSERT(get_queue(QueueType::LocalSequential)[ZoneValue(ztype)] == 0);
        DEBUG_ASSERT(zone != 0);

        get_queue(QueueType::LocalSequential)[ZoneValue(ztype)] = zone;
        push_back(zone, QueueType::LocalContiguous);
        push_back(zone, QueueType::SharedAffinity);
    }

    template <typename F>
    auto for_each_zone(F &&f) -> ustl::traits::InvokeResultT<F, PmZone &>;

    template <typename F, typename Predicate>
    auto for_each_zone_if(F &&f, Predicate &&p) -> ustl::traits::InvokeResultT<F, PmZone &>;

    /// Call it after init_queues
    auto connect(NodeMask const &nodemask) -> void;

    auto dump_queue(QueueType type) const -> void;

    FORCE_INLINE constexpr auto get_local_zone(ZoneType ztype) -> ZoneRef {
        DEBUG_ASSERT(ZoneValue(ztype) < NR_ZONES_PER_NODE);
        return get_queue(QueueType::LocalSequential)[ZoneValue(ztype)];
    }

    FORCE_INLINE constexpr auto get_queue(QueueType type) -> ZoneQueue {
        return queues_[type].subspan(0, get_queue_size(type));
    }

    FORCE_INLINE constexpr auto get_queue(QueueType type) const -> ZoneQueue {
        return queues_[type].subspan(0, get_queue_size(type));
    }

    FORCE_INLINE constexpr auto get_queue_size(QueueType type) const -> usize {
        return queue_sizes_[type];
    }

  private:
    auto init_queues() -> void;

    auto insert_remote_queue(ZoneQueue const &queue) -> void;

    template <QueueType Type>
    auto insert_remote_queue(ZoneQueue const &remote_queue) -> void;

    template <QueueType Type>
    auto priv_remove_zone(PmZone *zone) -> void;

    FORCE_INLINE constexpr auto push_back(PmZone *zone, QueueType type) -> void {
        queues_[type][queue_sizes_[type]] = zone;
        queue_sizes_[type] += 1;
    }

    constexpr static usize const kNumLocalQueues = SharedAffinity;

    constexpr static usize const kMaxZoneRef = {NUMA_MAX_NODE * (MaxNumQueues - kNumLocalQueues) +
                                                (NR_ZONES_PER_NODE * kNumLocalQueues)};

    ustl::Array<PmZone *, kMaxZoneRef> queue_storage_;
    usize queue_sizes_[MaxNumQueues];
    ZoneQueue queues_[MaxNumQueues];
    NodeMask connected_;
    NnId nid_;
};

/// `PmNode` is a class that describes a NUMA domain.
///
/// The memory regions between nodes should be mutually exclusive,
/// meaning that the memory region of one node should not overlap
/// with the memory region of another node.
class PmNode {
    using Self = PmNode;

  public:
    enum class State {
        Dma = usize(ZoneType::Dma),
        Dma32 = usize(ZoneType::Dma32),
        Normal = usize(ZoneType::Normal),
        Memory = Normal + 1,
        Online,
        Possible,
        NumStates,
    };

    class Current;

    template <typename F>
        requires ustl::traits::Invocable<F, NnId>
    static auto for_each_nid(State state, F &&f) -> ustl::traits::InvokeResultT<F, NnId> {
        return g_states[static_cast<usize>(state)].for_each(ustl::forward<F>(f));
    }

    template <typename F>
        requires ustl::traits::Invocable<F, Self &>
    static auto for_each_node(State state, F &&f) -> ustl::traits::InvokeResultT<F, Self &> {
        return g_states[static_cast<usize>(state)].for_each([&f](NnId nid) {
            DEBUG_ASSERT(Self::node(nid), "Touched an absent node in {}", __func__);
            return f(*Self::node(nid));
        });
    }

    FORCE_INLINE
    static auto node_online_mask() -> NodeMask const & {
        return g_states[static_cast<usize>(State::Online)];
    }

    FORCE_INLINE
    static auto node_possible_mask() -> NodeMask const & {
        return g_states[static_cast<usize>(State::Possible)];
    }

    FORCE_INLINE constexpr static auto node(NnId nid) -> PmNode * {
        return g_nodes[nid];
    }

    FORCE_INLINE constexpr static auto num_nodes() -> usize {
        return g_num_nodes;
    }

    FORCE_INLINE constexpr static auto num_online_nodes() -> usize {
        return g_num_online_nodes;
    }

    FORCE_INLINE constexpr static auto distance(NnId x, NnId y) -> usize {
        DEBUG_ASSERT(x < NUMA_MAX_NODE && y < NUMA_MAX_NODE, "");
        return g_node_distance[x][y];
    }

    FORCE_INLINE constexpr static auto set_distance(NnId x, NnId y, usize dis) -> void {
        DEBUG_ASSERT(x < NUMA_MAX_NODE && y < NUMA_MAX_NODE, "");
        g_node_distance[x][y] = dis;
    }

    static auto build_optimal_node_route(NnId from, NodeRoute &path, NodeMask const &filter)
        -> void;

    static auto bootstrap(ustl::views::Span<Pfn> max_zone_pfn) -> Status;

    [[nodiscard]]
    auto alloc_frames(Gaf gaf, usize order, NodeMask const &fallback)
        -> ustl::Result<PmFrame *, Status>;

    /// Allocates a series of physically contiguous memory fragments that collectively
    /// provide exactly |n| pages of memory. Instead of requiring the caller to manually
    /// calculate how to break down the request into different-sized fragments, this
    /// function automatically decomposes |n| into its binary representation and allocates
    /// appropriately sized contiguous chunks for each set bit.
    ///
    /// For example, if |n| = 5 (0b101), this function will allocate:
    /// - One fragment of size 2^0 = 1 page (for the least significant set bit)
    /// - One fragment of size 2^2 = 4 pages (for the next set bit)
    /// Together these provide exactly 5 pages of physically contiguous memory.
    ///
    /// This approach ensures optimal use of available memory by using the largest
    /// possible contiguous blocks first, minimizing fragmentation.
    ///
    /// Parameters:
    ///   - |gaf|: Allocation flags specifying memory type, constraints, etc.
    ///   - |n|: The exact number of pages to allocate, decomposed into binary components
    ///   - |fallback|: Fallback options if allocation fails in preferred regions
    ///
    /// Returns:
    ///   - On success: A list of PmFrame objects representing the allocated fragments.
    ///                 The list contains one frame for each set bit in the binary
    ///                 representation of |n|, with sizes corresponding to 2^bit_position.
    ///   - On failure: An error Status indicating the reason for allocation failure.
    [[nodiscard]]
    auto alloc_frames_fragment(Gaf gaf, usize n, NodeMask const &fallback)
        -> ustl::Result<PmFrame::List, Status>;

    /// Allocate a number of order-0 pages to a frame list.
    ///
    /// Return the frame list.
    [[nodiscard]]
    auto alloc_frames_bulk(Gaf gaf, usize n, NodeMask const &fallback)
        -> ustl::Result<PmFrame::List, Status>;

    /// Free a frame,
    auto free_frames(PmFrame *frame, usize order) -> void;

    /// Free a list of frame,
    auto free_frames(PmFrame::List *list) -> void;

    auto contains(usize order, ZoneType type) -> bool;

    auto dump() const -> void;

    FORCE_INLINE constexpr auto nid() const -> NnId {
        return m_id;
    }

    FORCE_INLINE constexpr auto work_cpus() const -> CpuMask const & {
        return m_work_cpus;
    }

    FORCE_INLINE constexpr auto hard_affinity() const -> CpuMask const & {
        return m_hard_affinity;
    }

    FORCE_INLINE constexpr auto soft_affinity() const -> CpuMask const & {
        return m_soft_affinity;
    }

    FORCE_INLINE constexpr auto zone_queues() -> ZoneQueues * {
        return &m_zone_queues;
    }

    FORCE_INLINE constexpr auto get_lock() -> sync::SpinLock & {
        return m_lock;
    }

  private:
    friend class ZoneQueues;

    static auto set_state(NnId nid, State state, bool on = true) -> void {
        g_states[static_cast<usize>(state)].set(nid, on);
    }

    PmNode(NnId nid);

    ~PmNode() = default;

    USTL_NO_MOVEABLE_AND_COPYABLE(PmNode);

    /// Initialize this node. The primary task involves verifying the existence of the
    /// specified PFN range.
    ///
    /// Requires:
    ///     1). Range [start, end) is sorted by address.
    auto init() -> Status;

    auto alloc_frames_slowpath(Gaf, usize, MemoryAllocationContext const &) -> PmFrame *;

    auto alloc_frames_direct_reclaim(Gaf, usize, AllocationControl, MemoryAllocationContext const &)
        -> PmFrame *;

    auto alloc_frames_direct_compact(Gaf, usize, AllocationControl, MemoryAllocationContext const &)
        -> PmFrame *;

    auto get_frame_from_zone_queues(Gaf, usize, AllocationControl, MemoryAllocationContext const &)
        -> PmFrame *;

    auto alloc_frames_preamble(Gaf, usize, MemoryAllocationContext *) -> Status;

    auto alloc_frames_postamble(PmFrame *, Gaf, usize, MemoryAllocationContext const &) -> void;

    [[no_unique_address]]
    gktl::Canary m_canary;

    NnId m_id;

    sync::SpinLock m_lock;

    /// The affinity based on hardware/software.
    /// Using rules:  (m_hard_affinity & m_soft_affinity)
    CpuMask m_hard_affinity TA_GUARD(get_lock());
    CpuMask m_soft_affinity TA_GUARD(get_lock());

    /// Which CPUs work on this node.
    CpuMask m_work_cpus TA_GUARD(get_lock());

    Pfn m_start_pfn;

    ustl::sync::AtomicUsize m_spanned_frames;

    //! @brief present_pages is physical pages existing within the zone,
    //! which is calculated as:
    //!	    present_frames_ = spanned_frames_ - absent_frames_(pages in holes).
    ustl::sync::AtomicUsize m_present_frames;

    ustl::sync::AtomicUsize m_reserved_frames;

    ZoneQueues m_zone_queues;
    PageQueues m_page_queues;

    [[no_unique_address]]
    MemoryAllocationPolicy m_node_policy;

    constexpr static usize const kNumStates = static_cast<usize>(State::NumStates);
    static ustl::Array<NodeMask, kNumStates> g_states;

    static ustl::Array<PmNode *, NUMA_MAX_NODE> g_nodes;

    // Th fields bottom is readonly after initializing logically.
    static ustl::Array<usize, NUMA_MAX_NODE, NUMA_MAX_NODE> g_node_distance;

    // Th fields bottom is readonly after initializing logically.
    static usize g_num_nodes;

    static usize g_num_online_nodes;

    static Gaf g_allowed_gaf;
};

/// The helper class be used to describe PmNode associated with current cpu context.
class PmNode::Current: public CpuLocal::Current<PmNode *> {
  public:
    static auto alloc_frames(Gaf gaf, usize order = 0,
                             NodeMask const &fallback = node_online_mask())
        -> ktl::Result<PmFrame *>;

    static auto alloc_frames_fragment(Gaf gaf, usize order = 0,
                                      NodeMask const &fallback = node_online_mask())
        -> ktl::Result<PmFrame::List>;

    static auto alloc_frames_bulk(Gaf gaf, usize n, NodeMask const &fallback = node_online_mask())
        -> ktl::Result<PmFrame::List>;

    FORCE_INLINE
    static auto hard_affinity() -> CpuMask const & {
        return CpuLocal::access(g_current_instance->m_hard_affinity);
    }

    FORCE_INLINE
    static auto nid() -> NnId {
        return CpuLocal::read(g_current_instance->m_id);
    }

    FORCE_INLINE
    static auto nid(CpuNum cpunum) -> NnId {
        return CpuLocal::access(g_current_instance->m_id, cpunum);
    }

    /// Used only on the early stage.
    FORCE_INLINE
    static auto set_nid_early(CpuNum cpunum, NnId nid) -> void {
        g_static_cpu_to_node[cpunum] = nid;
        g_dynamic_cpu_to_node[cpunum] = nid;
    }

    static auto set_nid(NnId nid) -> void;
    static auto set_nid(CpuNum cpunum, NnId nid) -> void;

    /// Used once by gktl::InitHook
    static auto init() -> void;

  private:
    friend class PmNode;

    static auto inherit_thread_context(Gaf &gaf) -> Status;

    PmNode *m_node;
    NodeMask m_hard_affinity;
    NodeMask m_soft_affinity;

    static ustl::Array<NnId, MAX_CPU> g_static_cpu_to_node;
    static ustl::Array<NnId, MAX_CPU> g_dynamic_cpu_to_node;
};

template <typename F, typename... Args>
auto NodeRoute::for_each_node(F &&f, Args &&...args)
    -> ustl::traits::InvokeResultT<F, PmNode &, Args...> {
    for (auto const nid: path_) {
        f(*PmNode::node(nid), ustl::forward<Args>(args)...);
    }
}

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_NODE_HPP