/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_NODE_HPP
#define OURS_MM_PM_NODE_HPP 1

#include <ours/assert.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/macro_abi.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/node_mask.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/priv/memory_allocation_context.hpp>
#include <ours/mm/priv/memory_allocation_policy.hpp>
#include <ours/sync/spin_lock.hpp>

#include <array>
#include <atomic>

#include <gktl/canary.hpp>
#include <ktl/result.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/collections/static_vec.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/function/fn.hpp>
#include <ustl/function/invoke.hpp>
#include <ustl/iterator/traits.hpp>
#include <ustl/mem/address_of.hpp>
#include <ustl/result.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/traits/is_same.hpp>
#include <ustl/util/pair.hpp>
#include <ustl/views/span.hpp>

namespace ours::mm {
struct NodeRoute {
    NodeRoute() : count_(), path_() {}

    NodeRoute(NodeRoute &&) = default;
    // Move-only: `path_` is a StaticVec, which deletes its own copy ctor.
    NodeRoute(NodeRoute const &) = delete;

    ~NodeRoute() = default;

    template <typename F, typename... Args>
    auto for_each_node(F &&f, Args &&...args) -> ustl::traits::InvokeResultT<F, PmNode &, Args...>;

    FORCE_INLINE
    constexpr auto set_next(NnId to) -> void {
        // path_ has a fixed capacity, so the only failure is overflow -- which
        // would also desynchronise count_ from it.
        auto const result = path_.push_back(to);
        DEBUG_ASSERT(result, "NodeRoute overflowed its {} hop capacity", NUMA_MAX_NODE - 1);
        count_ += 1;
    }

    FORCE_INLINE
    constexpr auto clear() -> void {
        path_.clear();
    }

    FORCE_INLINE
    constexpr auto begin() {
        return path_.begin();
    }

    FORCE_INLINE
    constexpr auto end() {
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

  public:
    // Public because pm-node.cpp's ZoneIterator is built on top of these.
    using ZoneRef = PmZone *;
    using ZoneQueue = ustl::views::Span<ZoneRef>;
    enum QueueType {
        /// A zone queue which arranged by zone type, there may be holes if a type of zone do not exist
        /// in the node. It's just useful when we free a frame.
        LocalSequential,
        LocalContiguous,
        SharedAffinity,
        SharedSequential,
        MaxNumQueues,
    };

    ZoneQueues(NnId nid) : queue_storage_(), queue_sizes_(), queues_(), connected_(), nid_(nid) {
        init_queues();
    }

    ~ZoneQueues() = default;

    FORCE_INLINE
    constexpr auto insert_local_zone(PmZone *zone, ZoneType ztype) -> void {
        DEBUG_ASSERT(static_cast<usize>(ztype) < NR_ZONES_PER_NODE);
        DEBUG_ASSERT(get_queue(QueueType::LocalSequential)[static_cast<usize>(ztype)] == 0);
        DEBUG_ASSERT(zone != 0);

        get_queue(QueueType::LocalSequential)[static_cast<usize>(ztype)] = zone;
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

    FORCE_INLINE
    constexpr auto get_local_zone(ZoneType ztype) -> ZoneRef {
        DEBUG_ASSERT(static_cast<usize>(ztype) < NR_ZONES_PER_NODE);
        return get_queue(QueueType::LocalSequential)[static_cast<usize>(ztype)];
    }

    FORCE_INLINE
    constexpr auto get_queue(QueueType type) -> ZoneQueue {
        return queues_[type].subspan(0, get_queue_size(type));
    }

    FORCE_INLINE
    constexpr auto get_queue(QueueType type) const -> ZoneQueue {
        return queues_[type].subspan(0, get_queue_size(type));
    }

    FORCE_INLINE
    constexpr auto get_queue_size(QueueType type) const -> usize {
        return queue_sizes_[type];
    }

  private:
    auto init_queues() -> void;

    auto insert_remote_queue(ZoneQueue const &queue) -> void;

    template <QueueType Type>
    auto insert_remote_queue(ZoneQueue const &remote_queue) -> void;

    template <QueueType Type>
    auto priv_remove_zone(PmZone *zone) -> void;

    FORCE_INLINE
    constexpr auto push_back(PmZone *zone, QueueType type) -> void {
        queues_[type][queue_sizes_[type]] = zone;
        queue_sizes_[type] += 1;
    }

    constexpr static usize NUM_LOCAL_QUEUES = SharedAffinity;

    constexpr static usize MAX_ZONE_REF = {NUMA_MAX_NODE * (MaxNumQueues - NUM_LOCAL_QUEUES) +
                                           (NR_ZONES_PER_NODE * NUM_LOCAL_QUEUES)};

    std::array<PmZone *, MAX_ZONE_REF> queue_storage_;
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
        return STATES[static_cast<usize>(state)].for_each(ustl::forward<F>(f));
    }

    template <typename F>
        requires ustl::traits::Invocable<F, Self &>
    static auto for_each_node(State state, F &&f) -> ustl::traits::InvokeResultT<F, Self &> {
        return STATES[static_cast<usize>(state)].for_each([&f](NnId nid) {
            DEBUG_ASSERT(Self::node(nid), "Touched an absent node in {}", __func__);
            return f(*Self::node(nid));
        });
    }

    FORCE_INLINE
    static auto node_online_mask() -> NodeMask const & {
        return STATES[static_cast<usize>(State::Online)];
    }

    FORCE_INLINE
    static auto node_possible_mask() -> NodeMask const & {
        return STATES[static_cast<usize>(State::Possible)];
    }

    /// Marks `nid` present in one of the state masks. The NUMA bring-up
    /// path (SRAT discovery) uses this to declare a domain Possible before
    /// the PMM builds its nodes.
    static auto set_state(NnId nid, State state, bool on = true) -> void {
        STATES[static_cast<usize>(state)].set(nid, on);
    }

    FORCE_INLINE
    constexpr static auto node(NnId nid) -> PmNode * {
        return NODES[nid];
    }

    FORCE_INLINE
    constexpr static auto num_nodes() -> usize {
        return NUM_NODES;
    }

    FORCE_INLINE
    constexpr static auto num_online_nodes() -> usize {
        return NUM_ONLINE_NODES;
    }

    FORCE_INLINE
    constexpr static auto distance(NnId x, NnId y) -> usize {
        DEBUG_ASSERT(x < NUMA_MAX_NODE && y < NUMA_MAX_NODE, "");
        return NODE_DISTANCE[x][y];
    }

    FORCE_INLINE
    constexpr static auto set_distance(NnId x, NnId y, usize dis) -> void {
        DEBUG_ASSERT(x < NUMA_MAX_NODE && y < NUMA_MAX_NODE, "");
        NODE_DISTANCE[x][y] = dis;
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

    /// Free a frame.
    ///
    /// Static because a frame goes back to the node that owns it, which is
    /// read out of the frame itself -- not to the node of whoever happens to
    /// be freeing it. There is nothing for an instance to contribute.
    static auto free_frames(PmFrame *frame, usize order) -> void;

    /// Free a list of frames, each back to its own node.
    static auto free_frames(PmFrame::List *list) -> void;

    auto contains(usize order, ZoneType type) -> bool;

    auto dump() const -> void;

    FORCE_INLINE
    constexpr auto nid() const -> NnId {
        return m_id;
    }

    /// A copy, taken under the node lock. The mask changes whenever a CPU is
    /// re-homed (`update_cpu_to_node`), so a reference into it would be a
    /// reference into a value the next re-homing rewrites under the caller;
    /// a copy is the only thing that can be handed out without the lock.
    auto work_cpus() -> CpuMask {
        Guard<sync::SpinLock, IrqSave> guard{get_lock()};
        return m_work_cpus;
    }

    FORCE_INLINE
    constexpr auto hard_affinity() const -> CpuMask const & {
        return m_hard_affinity;
    }

    FORCE_INLINE
    constexpr auto soft_affinity() const -> CpuMask const & {
        return m_soft_affinity;
    }

    FORCE_INLINE
    constexpr auto zone_queues() -> ZoneQueues * {
        return &m_zone_queues;
    }

    /// TA_RETURN_CAPABILITY ties the accessor to the member for the
    /// thread-safety analysis, so `TA_GUARDED(get_lock())` on the masks below and
    /// a `Guard<sync::SpinLock, IrqSave> guard{node->get_lock()}` at a call
    /// site name the same lock.
    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE constexpr auto get_lock() -> sync::SpinLock & {
        return m_lock;
    }

  private:
    friend class ZoneQueues;

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

    auto alloc_frames_direct_reclaim(Gaf, usize, AllocationOptions, MemoryAllocationContext const &)
        -> PmFrame *;

    auto alloc_frames_direct_compact(Gaf, usize, AllocationOptions, MemoryAllocationContext const &)
        -> PmFrame *;

    auto get_frame_from_zone_queues(Gaf, usize, AllocationOptions, MemoryAllocationContext const &)
        -> PmFrame *;

    /// Fills in the context the zone walk filters on.
    ///
    /// `fallback` is the caller's nodemask and has to reach the context, because
    /// get_frame_from_zone_queues() skips any zone whose node is not in
    /// `allowed_nodes` -- a context left default-constructed rejects every zone,
    /// and the slowpath then spins waiting for a reclaim that will never make
    /// the walk succeed.
    auto alloc_frames_preamble(Gaf gaf, usize order, NodeMask const &fallback,
                               MemoryAllocationContext *context) -> Status;

    auto alloc_frames_postamble(PmFrame *, Gaf, usize, MemoryAllocationContext const &) -> void;

    [[no_unique_address]]
    gktl::Canary m_canary;

    NnId m_id;

    /// Held with interrupts off. PmNode::Current::init() publishes every CPU
    /// under it at InitLevel::CpuLocal + 1, long before InitLevel::Threading
    /// gives the boot CPU a thread whose preemption could be held off, and
    /// work_cpus() reads it at run time; IrqSave is the option that is right
    /// in both.
    sync::SpinLock m_lock;

    /// The affinity based on hardware/software.
    /// Using rules:  (m_hard_affinity & m_soft_affinity)
    ///
    /// Not under m_lock: both are written once, in init() before the node is
    /// marked Online, and only read after that. The accessors hand out
    /// references and the per-CPU allocator caches a pointer to the hard
    /// mask for the life of the system, neither of which a lock could cover.
    /// They used to be TA_GUARDED(get_lock()), which no reader honoured and
    /// no reader could.
    CpuMask m_hard_affinity;
    CpuMask m_soft_affinity;

    /// Which CPUs work on this node. Unlike the two above this one does
    /// change after bring-up -- update_cpu_to_node() moves a CPU between
    /// nodes -- so it stays under the lock, and work_cpus() copies it out.
    CpuMask m_work_cpus TA_GUARDED(get_lock());

    Pfn m_start_pfn;

    std::atomic<usize> m_spanned_frames;

    //! @brief present_pages is physical pages existing within the zone,
    //! which is calculated as:
    //!	    present_frames_ = spanned_frames_ - absent_frames_(pages in holes).
    std::atomic<usize> m_present_frames;

    std::atomic<usize> m_reserved_frames;

    ZoneQueues m_zone_queues;
    PageQueues m_page_queues;

    [[no_unique_address]]
    MemoryAllocationPolicy m_node_policy;

    constexpr static usize NUM_STATES = static_cast<usize>(State::NumStates);
    static std::array<NodeMask, NUM_STATES> STATES;

    static std::array<PmNode *, NUMA_MAX_NODE> NODES;

    // Th fields bottom is readonly after initializing logically.
    static std::array<std::array<usize, NUMA_MAX_NODE>, NUMA_MAX_NODE> NODE_DISTANCE;

    // Th fields bottom is readonly after initializing logically.
    static usize NUM_NODES;

    static usize NUM_ONLINE_NODES;

    static Gaf ALLOWED_GAF;
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
        return CpuLocal::access(CURRENT_INSTANCE->m_hard_affinity);
    }

    /// The node the calling CPU belongs to.
    ///
    /// Read out of DYNAMIC_CPU_TO_NODE rather than through CURRENT_INSTANCE,
    /// and not as an optimisation. CURRENT_INSTANCE is a per-CPU `PmNode *`, so
    /// `CURRENT_INSTANCE->m_id` dereferences the *template* copy of that pointer
    /// at compile time -- it folds to `&(nullptr->m_id)`, i.e. the constant 4,
    /// which then gets handed to %gs as if it were a per-CPU handle. That is a
    /// wild access whichever model is in effect, and it stays wrong after the
    /// pointer is set, because the fold already happened.
    ///
    /// The array is also the only answer that works this early: nothing fills
    /// CURRENT_INSTANCE until PmNode::Current::init() runs from an init hook,
    /// while allocation starts asking for a node long before that. This is the
    /// same role Linux gives early_per_cpu(x86_cpu_to_node_map).
    FORCE_INLINE
    static auto nid() -> NnId {
        return nid(CpuLocal::current_cpu());
    }

    FORCE_INLINE
    static auto nid(CpuNum cpunum) -> NnId {
        DEBUG_ASSERT(cpunum < MAX_CPU, "Invalid CPU Number");
        return DYNAMIC_CPU_TO_NODE[cpunum];
    }

    /// Used only on the early stage.
    FORCE_INLINE
    static auto set_nid_early(CpuNum cpunum, NnId nid) -> void {
        STATIC_CPU_TO_NODE[cpunum] = nid;
        DYNAMIC_CPU_TO_NODE[cpunum] = nid;
    }

    static auto set_nid(NnId nid) -> void;
    static auto set_nid(CpuNum cpunum, NnId nid) -> void;

    /// Publishes every claimed CPU's node into its per-CPU slot. Run once,
    /// from an init hook, as soon as per-CPU storage exists; see the
    /// definition for why it does not go through set_nid().
    static auto init() -> void;

    /// Gives every CPU the firmware reported a node, and publishes it, before
    /// any of them starts. Run once, from an init hook after the MADT parse;
    /// see the definition for why bootstrap() could not do this itself.
    static auto claim_possible_cpus() -> void;

  private:
    friend class PmNode;

    static auto inherit_thread_context(Gaf &gaf) -> Status;

    /// Records |cpu| in its node's CPU masks and points its per-CPU slot at
    /// the node. Nothing for a CPU still on the NUMA_MAX_NODE sentinel.
    static auto publish(CpuNum cpu) -> void;

    PmNode *m_node;
    NodeMask m_hard_affinity;
    NodeMask m_soft_affinity;

    static std::array<NnId, MAX_CPU> STATIC_CPU_TO_NODE;
    static std::array<NnId, MAX_CPU> DYNAMIC_CPU_TO_NODE;
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