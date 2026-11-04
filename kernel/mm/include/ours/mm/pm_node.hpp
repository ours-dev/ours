/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_NODE_HPP
#define OURS_MM_PM_NODE_HPP 1

#include <ours/assert.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/priv/alloc_policy.hpp>
#include <ours/mm/priv/alloc_request.hpp>
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
/// A node's zones as its allocations see them: its own, which are its row of
/// PmZone's table, and the fallback order an allocation looks through -- this
/// node's own zones first, then every other node's, nearest first. Only the
/// fallback order is kept here. The node's own zones are read from the table,
/// their one record; a copy by type here was one fact stored twice, as the
/// next paragraph says of the queues before it.
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

    explicit ZoneList(NodeId nid) : m_fallback(), m_connected(), m_nid(nid) {}

    /// Puts this node's own zones, its row of the table, at the head of the
    /// fallback order, in type order -- before connect() appends anyone
    /// else's.
    auto take_local_zones() -> void {
        DEBUG_ASSERT(m_fallback.size() == 0, "node {}'s own zones taken twice, or after connect()",
                     m_nid);
        for_each_local_zone([this](PmZone &zone) { push_fallback(&zone); });
    }

    /// Chains the zones of every node in |order| behind this node's own, in
    /// the order given.
    ///
    /// A list of node ids rather than a set: it used to take the set and
    /// derive the order itself, from the machine's distance matrix, which made
    /// "which nodes" and "in what order" one argument and put a NUMA concept
    /// inside the allocator's data structure. The derivation is
    /// numa::build_fallback_orders() now, and this appends what it was handed.
    auto connect(ustl::views::Span<NodeId const> order) -> void;

    /// This node's zone of |ztype|, or null when the node has none.
    [[nodiscard]]
    FORCE_INLINE
    auto local_zone(ZoneType ztype) const -> ZoneRef {
        return PmZone::at(m_nid, ztype);
    }

    /// This node's own zones in type order, skipping the types it has none of.
    /// What LocalContiguous used to be kept for.
    template <typename F>
        requires ustl::traits::Invocable<F, PmZone &>
    auto for_each_local_zone(F &&f) const -> void {
        for (auto *const zone: PmZone::row(m_nid)) {
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

    /// The node's row of the table, by type, holes included, for a walk that
    /// filters as it goes.
    [[nodiscard]]
    FORCE_INLINE
    auto local_zones() const -> Zones {
        return PmZone::row(m_nid);
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
    /// Whether a node has memory of a zone type is not a state: it is whether
    /// PmZone::at() has a zone for it.
    enum class State : u32 {
        Memory,
        Online,
        Possible,
        NumStates,
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

    /// The nodes that own memory -- the ones worth falling back to, and so the
    /// members the fallback order is derived over.
    FORCE_INLINE
    static auto node_memory_mask() -> NodeMask const & {
        return STATES[std::to_underlying(State::Memory)];
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

    /// Records |cpu| as working on the node the placement table puts it on,
    /// and caches that node in |cpu|'s slot. Nothing for a CPU still on the
    /// MAX_NODES sentinel.
    static auto publish_cpu(CpuNum cpu) -> void;

    /// publish_cpu() for every CPU claimed so far. Run once, from an init
    /// hook, as soon as per-CPU storage exists; see the definition for why it
    /// is unconditional rather than going through set_cpu_node().
    static auto publish_cpus() -> void;

    /// Gives every CPU the firmware reported a node, and publishes it, before
    /// any of them starts. Run once, from an init hook after the MADT parse;
    /// see the definition for why bootstrap() could not do this itself.
    static auto claim_possible_cpus() -> void;

    /// Moves |cpu| to node |nid|: out of its old node's CPU mask, into the new
    /// one's, and the placement table and its cache along with them.
    ///
    /// No caller yet. It is kept because it is the one place that knows what a
    /// move has to touch, and it is three objects wide -- two nodes' masks,
    /// each under its own lock, and the placement -- so the next thing that
    /// re-homes a CPU should not have to rediscover the order.
    static auto set_cpu_node(CpuNum cpu, NodeId nid) -> void;

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
    auto alloc_frames(AllocFlags alloc_flags, usize order, NodeMask const &fallback)
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
    ///   - |alloc_flags|: Allocation flags specifying memory type, constraints, etc.
    ///   - |n|: The exact number of pages to allocate, decomposed into binary components
    ///   - |fallback|: Fallback options if allocation fails in preferred regions
    ///
    /// Returns:
    ///   - On success: A list of PmFrame objects representing the allocated fragments.
    ///                 The list contains one frame for each set bit in the binary
    ///                 representation of |n|, with sizes corresponding to 2^bit_position.
    ///   - On failure: An error Status indicating the reason for allocation failure.
    [[nodiscard]]
    auto alloc_frames_fragment(AllocFlags alloc_flags, usize n, NodeMask const &fallback)
        -> ustl::Result<PmFrame::List, Status>;

    /// Allocate a number of order-0 pages to a frame list.
    ///
    /// Return the frame list.
    [[nodiscard]]
    auto alloc_frames_bulk(AllocFlags alloc_flags, usize n, NodeMask const &fallback)
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

    /// The queues this node's VMO pages are filed on, by kind and by age.
    FORCE_INLINE
    auto page_queues() -> PageQueues & {
        return m_page_queues;
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

    /// Held with interrupts off. PmNode::publish_cpus() publishes every CPU
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
    AllocPolicy m_node_policy;

    constexpr static usize NUM_STATES = std::to_underlying(State::NumStates);
    static std::array<NodeMask, NUM_STATES> STATES;

    static std::array<PmNode *, MAX_NODES> NODES;

    // Th fields bottom is readonly after initializing logically.
    static usize NUM_NODES;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_NODE_HPP
