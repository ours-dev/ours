/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_NODE_HPP
#define OURS_MM_PM_NODE_HPP 1

#include <ours/assert.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/priv/alloc_request.hpp>
#include <ours/mm/priv/memory_allocation_policy.hpp>
#include <ours/node_mask.hpp>
#include <ours/sync/spin_lock.hpp>

#include <array>
#include <atomic>
#include <utility>

#include <gktl/canary.hpp>
#include <ktl/result.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/collections/static_vec.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/function/invoke.hpp>
#include <ustl/iterator/traits.hpp>
#include <ustl/mem/address_of.hpp>
#include <ustl/result.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/traits/is_same.hpp>
#include <ustl/util/pair.hpp>
#include <ustl/views/span.hpp>

namespace ours::mm {
/// A node's zones, kept two ways: by type, which is how a freed frame finds the
/// zone it came from, and in fallback order, which is how an allocation looks
/// for one -- this node's own zones first, then every other node's, nearest
/// first.
///
/// It used to be ZoneQueues, four queues cut out of one shared array with
/// subspan() and a length kept for each: LocalSequential (by type, with holes),
/// LocalContiguous (the same zones without the holes), SharedAffinity (the
/// fallback order, the only one an allocation walked) and SharedSequential
/// (never filled). The two local ones were one fact stored twice; the fallback
/// order is the only list that is not derived from the by-type array.
class ZoneList {
    using Self = ZoneList;

  public:
    using ZoneRef = PmZone *;
    using Zones = ustl::views::Span<ZoneRef const>;

    explicit ZoneList(NodeId nid) : m_zones(), m_fallback(), m_connected(), m_nid(nid) {}

    /// Adds one of this node's own zones, under its type and at the end of
    /// the fallback order -- before connect() appends anyone else's.
    FORCE_INLINE
    auto insert_local_zone(PmZone *zone, ZoneType ztype) -> void {
        auto const index = std::to_underlying(ztype);
        DEBUG_ASSERT(index < NR_ZONES_PER_NODE);
        DEBUG_ASSERT(!m_zones[index], "zone type {} inserted twice", index);
        DEBUG_ASSERT(zone);

        m_zones[index] = zone;
        push_fallback(zone);
    }

    /// Chains the zones of every node in |nodemask| behind this node's own,
    /// nearest node first.
    auto connect(NodeMask const &nodemask) -> void;

    /// This node's zone of |ztype|, or null when the node has none.
    FORCE_INLINE
    auto local_zone(ZoneType ztype) const -> ZoneRef {
        DEBUG_ASSERT(std::to_underlying(ztype) < NR_ZONES_PER_NODE);
        return m_zones[std::to_underlying(ztype)];
    }

    /// This node's own zones in type order, skipping the types it has none of.
    /// What LocalContiguous used to be kept for.
    template <typename F>
        requires ustl::traits::Invocable<F, PmZone &>
    auto for_each_local_zone(F &&f) const -> void {
        for (auto *const zone: m_zones) {
            if (zone) {
                f(*zone);
            }
        }
    }

    /// Visits the zones in fallback order. A callback returning void visits
    /// every zone; one returning a value stops at the first result that tests
    /// true and yields it.
    template <typename F>
    auto for_each_zone(F &&f) -> ustl::traits::InvokeResultT<F, PmZone &>;

    template <typename F, typename Predicate>
    auto for_each_zone_if(F &&f, Predicate &&p) -> ustl::traits::InvokeResultT<F, PmZone &>;

    /// The by-type array, holes included, for a walk that filters as it goes.
    FORCE_INLINE
    auto local_zones() const -> Zones {
        return Zones{m_zones.data(), m_zones.size()};
    }

    /// The fallback order.
    FORCE_INLINE
    auto fallback() const -> Zones {
        return Zones{m_fallback.data(), m_fallback.size()};
    }

    auto dump_fallback() const -> void;

  private:
    FORCE_INLINE
    auto push_fallback(PmZone *zone) -> void {
        auto const pushed = m_fallback.push_back(zone);
        DEBUG_ASSERT(pushed, "fallback order overflowed its {} zones", MAX_ZONES);
    }

    std::array<ZoneRef, NR_ZONES_PER_NODE> m_zones;
    ustl::collections::StaticVec<ZoneRef, MAX_ZONES> m_fallback;

    /// The remote nodes connect() chained into the fallback order.
    NodeMask m_connected;
    NodeId m_nid;
};

/// `PmNode` is a class that describes a NUMA domain.
///
/// The memory regions between nodes should be mutually exclusive,
/// meaning that the memory region of one node should not overlap
/// with the memory region of another node.
class PmNode {
    using Self = PmNode;

  public:
    enum class State : u32 {
        Dma = usize(ZoneType::Dma),
        Dma32 = usize(ZoneType::Dma32),
        Normal = usize(ZoneType::Normal),
        Memory = Normal + 1,
        Online,
        Possible,
        NumStates,
    };

    class Current;

    /// The other nodes with memory, nearest first, as seen from one node: the
    /// order that node's allocations fall back through. ZoneList::connect()
    /// chains their zones behind the node's own in this order.
    ///
    /// Nested because it is part of how a node is wired up; its one user in
    /// the allocator is ZoneList::connect(). Public so that pmnode.test can
    /// check the order it produces directly. It was a free-standing NodeRoute that the caller created empty
    /// and handed to PmNode::build_optimal_node_route() to fill; build() now
    /// returns a finished one.
    class Router {
      public:
        /// Every node with memory in |candidates| other than |from|, nearest
        /// first.
        static auto build(NodeId from, NodeMask const &candidates) -> Router;

        template <typename F, typename... Args>
            requires ustl::traits::Invocable<F, PmNode &, Args...>
        auto for_each_node(F &&f, Args &&...args) const -> void;

        FORCE_INLINE
        constexpr auto clear() -> void {
            m_path.clear();
        }

        FORCE_INLINE
        constexpr auto begin() const {
            return m_path.begin();
        }

        FORCE_INLINE
        constexpr auto end() const {
            return m_path.end();
        }

      private:
        /// Appends the next hop. Fixed capacity: one hop per other node, so
        /// overflowing it means a node was visited twice.
        FORCE_INLINE
        constexpr auto push_back(NodeId to) -> void {
            auto const result = m_path.push_back(to);
            DEBUG_ASSERT(result, "Router overflowed its {} hop capacity", MAX_NODES - 1);
        }

        /// The hops, and their count: StaticVec keeps both.
        ustl::collections::StaticVec<NodeId, MAX_NODES - 1> m_path;
    };

    template <typename F>
        requires ustl::traits::Invocable<F, NodeId>
    static auto for_each_nid(State state, F &&f) -> ustl::traits::InvokeResultT<F, NodeId> {
        return STATES[std::to_underlying(state)].for_each(ustl::forward<F>(f));
    }

    template <typename F>
        requires ustl::traits::Invocable<F, Self &>
    static auto for_each_node(State state, F &&f) -> ustl::traits::InvokeResultT<F, Self &> {
        return STATES[std::to_underlying(state)].for_each([&f](NodeId nid) {
            DEBUG_ASSERT(Self::node(nid), "Touched an absent node in {}", __func__);
            return f(*Self::node(nid));
        });
    }

    FORCE_INLINE
    static auto node_online_mask() -> NodeMask const & {
        return STATES[std::to_underlying(State::Online)];
    }

    FORCE_INLINE
    static auto node_possible_mask() -> NodeMask const & {
        return STATES[std::to_underlying(State::Possible)];
    }

    /// Marks `nid` present in one of the state masks. The NUMA bring-up
    /// path (SRAT discovery) uses this to declare a domain Possible before
    /// the PMM builds its nodes.
    static auto set_state(NodeId nid, State state, bool on = true) -> void {
        STATES[std::to_underlying(state)].set(nid, on);
    }

    FORCE_INLINE
    constexpr static auto node(NodeId nid) -> PmNode * {
        return NODES[nid];
    }

    FORCE_INLINE
    constexpr static auto num_nodes() -> usize {
        return NUM_NODES;
    }

    /// Counted off the Online mask. It used to return a NUM_ONLINE_NODES
    /// counter that nothing ever incremented, so it answered 0 on every
    /// machine; one fact kept in two places had drifted, and the mask is the
    /// one init() actually maintains.
    FORCE_INLINE
    static auto num_online_nodes() -> usize {
        return node_online_mask().count();
    }

    static auto bootstrap(ustl::views::Span<Pfn> max_zone_pfn) -> Status;

    /// Works out how many frames the machine holds back from allocation and
    /// gives every zone its share of it as three watermarks.
    ///
    /// Separate from bootstrap(), and later than it, because it needs numbers
    /// bootstrap() cannot have yet: a zone's managed total is what is present
    /// minus what the boot allocator took, and the reservations are only
    /// counted once init_memmap() has walked them. Called from init_pmm()
    /// after the handoff, with nothing else running.
    static auto setup_watermarks() -> void;

    [[nodiscard]]
    auto alloc_frames(Gaf gaf, usize order, NodeMask const &fallback)
        -> ustl::Result<PmFrame *, Status>;

    /// Allocates exactly |n| pages as a list of blocks, one per set bit of |n|:
    /// bit k contributes one block of 2^k contiguous pages. Each block is
    /// contiguous in itself; the blocks are not contiguous with each other.
    ///
    /// For example, |n| = 5 (0b101) yields a 1-page block and a 4-page block.
    ///
    /// The blocks are taken smallest first. On failure every block already
    /// taken is freed and the error returned.
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

    /// Whether this node's zone of |type| could serve an allocation of
    /// |order| from its free lists right now; false when the node has no such
    /// zone. A snapshot, like PmZone::contains(), which answers it.
    ///
    /// It used to be declared and never defined.
    [[nodiscard]]
    auto contains(usize order, ZoneType type) -> bool;

    auto dump() const -> void;

    FORCE_INLINE
    constexpr auto nid() const -> NodeId {
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
    constexpr auto zone_list() -> ZoneList * {
        return &m_zone_list;
    }

    /// TA_RETURN_CAPABILITY ties the accessor to the member for the
    /// thread-safety analysis, so `TA_GUARDED(get_lock())` on the masks below and
    /// a `Guard<sync::SpinLock, IrqSave> guard{node->get_lock()}` at a call
    /// site name the same lock.
    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE constexpr auto get_lock() -> sync::SpinLock & {
        return m_lock;
    }

  private:
    friend class ZoneList;

    PmNode(NodeId nid);

    ~PmNode() = default;

    USTL_NO_MOVEABLE_AND_COPYABLE(PmNode);

    /// Creates and registers a PmNode for every node in |nodes|; one of
    /// bootstrap()'s steps. See pm_node_init.cpp.
    static auto create_nodes(NodeMask const &nodes) -> Status;

    /// Initialize this node: its span, its zones, its hard affinity. The
    /// primary task involves verifying the existence of the specified PFN
    /// range. Called by bootstrap() only; defined in pm_node_init.cpp.
    ///
    /// Requires:
    ///     1). Range [start, end) is sorted by address.
    auto init() -> Status;

    auto get_frame_from_zone_list(AllocRequest const &request) -> PmFrame *;

    auto alloc_frames_postamble(PmFrame *, AllocRequest const &) -> void;

    [[no_unique_address]]
    gktl::Canary m_canary;

    NodeId m_id;

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

    ZoneList m_zone_list;
    PageQueues m_page_queues;

    [[no_unique_address]]
    MemoryAllocationPolicy m_node_policy;

    constexpr static usize NUM_STATES = std::to_underlying(State::NumStates);
    static std::array<NodeMask, NUM_STATES> STATES;

    static std::array<PmNode *, MAX_NODES> NODES;

    // Th fields bottom is readonly after initializing logically.
    static usize NUM_NODES;
};


/// The helper class be used to describe PmNode associated with current cpu context.
class PmNode::Current: public CpuLocal::Current<PmNode *> {
  public:
    static auto alloc_frames(Gaf gaf, usize order = 0,
                             NodeMask const &fallback = node_online_mask())
        -> ktl::Result<PmFrame *>;

    static auto alloc_frames_fragment(Gaf gaf, usize n,
                                      NodeMask const &fallback = node_online_mask())
        -> ktl::Result<PmFrame::List>;

    static auto alloc_frames_bulk(Gaf gaf, usize n, NodeMask const &fallback = node_online_mask())
        -> ktl::Result<PmFrame::List>;

    FORCE_INLINE
    static auto hard_affinity() -> CpuMask const & {
        return CpuLocal::access(CURRENT_INSTANCE->m_hard_affinity);
    }

    static auto set_nid(NodeId nid) -> void;
    static auto set_nid(CpuNum cpunum, NodeId nid) -> void;

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

    /// Where the calling thread's memory policy says its frames should come
    /// from. Only for the node-less entries: a caller that names a node has
    /// already said where, which is why PmNode::alloc_frames() does not apply
    /// it. Whether the thread may allocate at all is a different question,
    /// asked in PmNode::alloc_frames() for every caller.
    static auto apply_thread_policy(Gaf &gaf) -> Status;

    /// apply_thread_policy() and resolve_local_node() together: what each
    /// node-less alloc_* entry does before handing the request to a node. The
    /// calling CPU's node, or why there is none.
    static auto local_node_for(Gaf &gaf) -> ktl::Result<PmNode *>;

    /// The calling CPU's PmNode, answered even before CURRENT_INSTANCE exists.
    ///
    /// The per-CPU pointer is filled by Current::init(), an init hook at
    /// InitLevel::CpuLocal + 1 -- i.e. after CpuLocal::init(), which allocates
    /// the per-CPU units themselves and the page tables that map them. Read
    /// before that, `get()` answers null, and a null PmNode is not a failure
    /// the allocation paths below notice on their own: they walk straight into
    /// it and read a ZoneList out of low memory, which phys's identity map
    /// still makes readable, so the wreck surfaces somewhere else entirely.
    /// That has happened twice -- once on init_vmm_preheap()'s zero-page
    /// allocation (see the comment on publish()), once as a #GP inside
    /// CpuLocal::init() -- which is why the resolution lives here rather than
    /// in each caller.
    ///
    /// NumaTopology::current_node() is what makes an answer possible this early:
    /// it reads Topology's table, which bootstrap() fills for the boot CPU long
    /// before any per-CPU storage exists.
    static auto resolve_local_node() -> PmNode *;

    /// Records |cpu| in its node's CPU masks and points its per-CPU slot at
    /// the node. Nothing for a CPU still on the MAX_NODES sentinel.
    static auto publish(CpuNum cpu) -> void;

    // Not read yet. Kept as planned per-CPU state (the node, and the
    // affinities an allocation from this CPU should honour); nothing
    // constructs a Current instance today, which is why the compiler first
    // noticed m_node once Current lost its static tables to Topology.
    [[maybe_unused]]
    PmNode *m_node;
    NodeMask m_hard_affinity;
    NodeMask m_soft_affinity;
};

template <typename F, typename... Args>
    requires ustl::traits::Invocable<F, PmNode &, Args...>
auto PmNode::Router::for_each_node(F &&f, Args &&...args) const -> void {
    // void, not the callback's result: every node is visited and nothing is
    // collected.
    for (auto const nid: m_path) {
        f(*PmNode::node(nid), ustl::forward<Args>(args)...);
    }
}

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_NODE_HPP