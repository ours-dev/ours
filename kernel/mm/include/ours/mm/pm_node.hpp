/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_NODE_HPP
#define OURS_MM_PM_NODE_HPP 1

#include <ours/assert.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/init.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/node_table.hpp>
#include <ours/mm/page_queues.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/priv/alloc_policy.hpp>
#include <ours/mm/priv/alloc_request.hpp>
#include <ours/mm/priv/zone_candidates.hpp>
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
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/traits/is_same.hpp>
#include <ustl/util/pair.hpp>
#include <ustl/views/span.hpp>

namespace ours::mm {
class CpuNodeMap;
class PmNode;

/// A pass to a node's CPU bookkeeping (PmNode::add_cpu(), remove_cpu(),
/// join_hard_affinity()). Which node a CPU works on is CpuNodeMap's table,
/// and CpuNodeMap is what keeps the table, the nodes' masks and the per-CPU
/// cache in step, so it is the one that can make a key; the node itself makes
/// one too, for the hard affinity it builds when it is made. A friend would
/// open every private member; the key opens these three calls.
class CpuPlacementKey {
    friend class CpuNodeMap;
    friend class PmNode;
    CpuPlacementKey() = default;
};

/// A node's zones as its allocations see them: its own, which are its row of
/// PmZone's table, and the fallback order an allocation looks through -- this
/// node's own zones first, then every other node's, nearest first, and each
/// node's from its highest zone type down. Only the
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

    explicit ZoneList(NodeId nid) : m_fallback(), m_nid(nid) {}

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

    /// Visits the zones in fallback order: this node's own first, then the
    /// remote ones in the order connect() was given. A callback returning
    /// void visits every zone; one returning a value stops at the first
    /// result that tests true and yields it, which makes this "take from the
    /// first zone that can spare it" rather than only a traversal.
    ///
    /// Defined here, in the header, because a template defined in one
    /// source file is not there for a caller in any other.
    template <typename F>
    auto for_each_zone(F &&f) const -> ustl::traits::InvokeResultT<F, PmZone &> {
        using Result = ustl::traits::InvokeResultT<F, PmZone &>;
        if constexpr (std::is_void_v<Result>) {
            for (auto *const zone: m_fallback) {
                f(*zone);
            }
        } else {
            for (auto *const zone: m_fallback) {
                if (auto result = f(*zone)) {
                    return result;
                }
            }
            return Result{};
        }
    }

    /// for_each_zone() over the zones |p| accepts.
    template <typename F, typename Predicate>
    auto for_each_zone_if(F &&f, Predicate &&p) const -> ustl::traits::InvokeResultT<F, PmZone &> {
        return for_each_zone([&](PmZone &zone) -> ustl::traits::InvokeResultT<F, PmZone &> {
            if (p(zone)) {
                return f(zone);
            }
            return {};
        });
    }

    /// The node's row of the table, by type, holes included, for a walk that
    /// filters as it goes.
    [[nodiscard]]
    FORCE_INLINE
    auto local_zones() const -> Zones {
        return PmZone::row(m_nid);
    }

    /// The fallback order.
    [[nodiscard]]
    FORCE_INLINE
    auto fallback() const -> Zones {
        return Zones{m_fallback.data(), m_fallback.size()};
    }

    /// Eligible zones in fallback order. Availability is decided by each zone.
    [[nodiscard]]
    FORCE_INLINE
    auto candidates(AllocRequest const &request) const -> ZoneCandidates {
        return ZoneCandidates{fallback(), request};
    }

    auto dump_fallback() const -> void;

  private:
    /// Only the node that owns the list builds it, at bring-up: every
    /// allocation walks the fallback order without a lock, on the premise
    /// that nothing writes it after boot, and the two builders being reachable
    /// from the node alone is what keeps that premise.
    friend class PmNode;

    /// Puts this node's own zones, its row of the table, at the head of the
    /// fallback order, from the highest type down -- before connect() appends
    /// anyone else's.
    auto take_local_zones() -> void {
        DEBUG_ASSERT(m_fallback.size() == 0, "node {}'s own zones taken twice, or after connect()",
                     m_nid);
        push_zones_of(m_nid);
    }

    /// Chains the zones of every node in |order| behind this node's own, in
    /// the order given, each node's from its highest type down.
    ///
    /// A list of node ids rather than a set: it used to take the set and
    /// derive the order itself, from the machine's distance matrix, which made
    /// "which nodes" and "in what order" one argument and put a NUMA concept
    /// inside the allocator's data structure. The derivation is
    /// numa::build_fallback_orders() now, and this appends what it was handed.
    auto connect(ustl::views::Span<NodeId const> order) -> void;

    FORCE_INLINE
    auto push_fallback(PmZone *zone) -> void {
        auto const pushed = m_fallback.push_back(zone);
        DEBUG_ASSERT(pushed, "fallback order overflowed its {} zones", MAX_ZONES);
    }

    /// Appends node |nid|'s zones to the fallback order from its highest type
    /// down, skipping the types it has none of.
    ///
    /// Highest first because the zones are not interchangeable in the other
    /// direction. Any request may be served from a high zone, but only a low
    /// one can serve a request with an address limit (a device that reaches
    /// only the first 16 MiB, or 4 GiB). Taking ordinary requests from the
    /// low zones first spends the only memory that has no substitute while
    /// the memory that has one sits unused. Node first, zone second: a
    /// node's low zones are still tried before the next node's high ones,
    /// because a frame on another node costs every access, and a low zone
    /// spent costs only a later constrained request.
    auto push_zones_of(NodeId const nid) -> void {
        auto const row = PmZone::row(nid);
        for (auto type = row.size(); type > 0; --type) {
            if (auto *const zone = row[type - 1]) {
                push_fallback(zone);
            }
        }
    }

    ustl::collections::StaticVec<ZoneRef, MAX_ZONES> m_fallback;
    NodeId m_nid;
};

/// `PmNode` is a class that describes a NUMA domain: its zones and the order
/// an allocation tries them in, its page queues, and the CPUs working on it.
/// Which nodes exist is not a node's: see ours/mm/node_table.hpp.
///
/// The memory regions between nodes should be mutually exclusive,
/// meaning that the memory region of one node should not overlap
/// with the memory region of another node.
class PmNode {
    using Self = PmNode;

  public:
    static auto bootstrap(ustl::views::Span<Pfn> max_zone_pfn) -> Status;

    /// Why a walk for one block gave nothing: no zone both admitted the request
    /// at the pass's floor and had a block. What status that becomes is the
    /// caller's (ShouldWait after a Low pass a waiting request stops at,
    /// OutOfMem after the Min pass), so the failure carries only the first
    /// zone the walk passed that is short, if any, for whoever asks the
    /// reclaimer. A zone is short when it did not admit the request (taking
    /// the block would have left it below the pass's floor, so a large block
    /// can find a zone short that a single frame would not), or when an
    /// order-0 request found no frame in it while it was below its high
    /// watermark. A zone with no free block of a larger order is not short on
    /// that account alone: it may still have plenty of single frames.
    struct BlockFailure {
        ustl::Option<PmZone const &> depleted_zone = ustl::NONE;
    };

    /// The node's layer of mm::alloc_frames(): walks this node's zones in
    /// fallback order for |request|, whose home this node is, and takes one
    /// block from the first zone that admits the request in |pass| and has
    /// the block. A zone that does not admit it is passed over and, the first
    /// time, named in the failure as short. That is all of it: the block
    /// comes back as the zone gave it, and making it ready for its owner is
    /// the caller's, as are the free-memory gate, the thread's allocation
    /// state and the request itself. resolve_request() builds a request and the mm::
    /// functions are the callers meant to; both it and this member are
    /// public, and a caller that builds its own request and comes here skips
    /// the gate, the allocation-state check, the order bound, the second
    /// pass and prepare_alloc(); the watermark of the pass it names still
    /// holds. So it takes a key only the pipeline and tests can make
    /// (NodeLayerKey).
    [[nodiscard]]
    auto alloc_frames(AllocRequest const &request, AdmissionPass pass, NodeLayerKey)
        -> ustl::Result<PmFrame *, BlockFailure>;

    /// The node's layer of mm::free_frames(): returns a block of this node's
    /// to the zone it came from. The frame names both, and its head the
    /// block's order. Returns what the
    /// zone's free_frames() does: whether this release brought the zone back
    /// to admitting a frame at its Low watermark. Takes a NodeLayerKey, which
    /// only the pipeline and tests make: the pipeline checks the frame
    /// (prepare_free()) before this call, and wakes one waiter after it when
    /// this returns true.
    [[nodiscard]]
    auto free_frames(PmFrame &frame, NodeLayerKey) -> bool;

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
    /// re-homed (CpuNodeMap::move()), so a reference into it would be a
    /// reference into a value the next re-homing rewrites under the caller;
    /// a copy is the only thing that can be handed out without the lock.
    auto work_cpus() -> CpuMask {
        Guard<sync::SpinLock, IrqSave> guard{get_lock()};
        return m_work_cpus;
    }

    /// Records |cpu| as working on this node, and remove_cpu() takes it out
    /// again. Each takes this node's lock for this node's mask and nothing
    /// else: which node a CPU is on is CpuNodeMap's table, and CpuNodeMap is
    /// the caller, which keeps the table, the two nodes' masks and the
    /// per-CPU cache in step.
    auto add_cpu(CpuNum cpu, CpuPlacementKey) -> void;
    auto remove_cpu(CpuNum cpu, CpuPlacementKey) -> void;

    /// Adds |cpu| to the hard affinity if the firmware placed it on this
    /// node. init() already does this for every CPU the firmware's table
    /// names when the node is made, and today the table is complete by then
    /// (the SRAT parse fills it at PrePmm + 1, before any node exists), so
    /// this adds nothing. It keeps publishing a CPU doing what it always did,
    /// for a firmware table that is filled later.
    ///
    /// For the init hooks alone, which run on the boot CPU before any other
    /// CPU is started: the mask is read without a lock for the rest of the
    /// system's life, so nothing may write it once another CPU can read it.
    /// A CPU moved between nodes at run time does not come here. The hard
    /// affinity is what the firmware said, and does not follow a move.
    auto join_hard_affinity(CpuNum cpu, CpuPlacementKey) -> void;

    FORCE_INLINE
    constexpr auto hard_affinity() const -> CpuMask const & {
        return m_hard_affinity;
    }

    FORCE_INLINE
    constexpr auto soft_affinity() const -> CpuMask const & {
        return m_soft_affinity;
    }

    /// The node's zones and the order its allocations try them in, to read.
    /// Only the node builds the list, at bring-up (connect_all()); a caller
    /// gets it const, so nothing can rewrite the order the allocations walk
    /// without a lock.
    FORCE_INLINE
    constexpr auto zone_list() const -> ZoneList const & {
        return m_zone_list;
    }

    /// The queues this node's VMO pages are filed on, by kind and by age.
    FORCE_INLINE
    auto page_queues() -> PageQueues & {
        return m_page_queues;
    }

    /// Marks |zone|, one of this node's, as short: an allocation found it
    /// below its floor and wants it reclaimed. True when this is the first
    /// mark since take_reclaim_target() last took the node's record, and the
    /// caller then wakes whatever takes it; false when an earlier mark is
    /// still waiting, whose wake has gone out, or goes out when the reclaimer
    /// starts or the last Pause ends. The node only records: it wakes nothing
    /// itself, so it does not depend on the reclaimer above it (the pipeline's
    /// observe_pressure() is the caller, and signals the event the reclaimer
    /// registered with set_reclaim_signal()).
    [[nodiscard]]
    auto mark_short(PmZone const &zone) -> bool;

    /// Takes the node's record of short zones, and answers how many frames
    /// the marked ones are short of High, read now: zero when nothing was
    /// marked or the marked zones have recovered since. The take clears the
    /// record, so each mark is answered once.
    [[nodiscard]]
    auto take_reclaim_target() -> usize;

    /// TA_RETURN_CAPABILITY ties the accessor to the member for the
    /// thread-safety analysis, so `TA_GUARDED(get_lock())` on the masks below and
    /// a `Guard<sync::SpinLock, IrqSave> guard{node->get_lock()}` at a call
    /// site name the same lock.
    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE constexpr auto get_lock() -> sync::SpinLock & {
        return m_lock;
    }

  private:
    PmNode(NodeId nid);

    ~PmNode() = default;

    USTL_NO_MOVEABLE_AND_COPYABLE(PmNode);

    /// Creates and registers a PmNode for every node in |nodes|; one of
    /// bootstrap()'s steps. See pm_node_init.cpp.
    static auto create_nodes(NodeMask const &nodes) -> Status;

    /// Gives every node in |nodes| its fallback order, from one derivation
    /// over the machine; one of bootstrap()'s steps. See pm_node_init.cpp.
    static auto connect_all(NodeMask const &nodes) -> void;

    /// Initialize this node: its span, its zones, its hard affinity. The
    /// primary task involves verifying the existence of the specified PFN
    /// range. Called by bootstrap() only; defined in pm_node_init.cpp.
    ///
    /// Requires:
    ///     1). Range [start, end) is sorted by address.
    auto init() -> Status;

    [[no_unique_address]]
    gktl::Canary m_canary;

    NodeId m_id;

    /// Held with interrupts off. CpuNodeMap::publish_all() publishes every CPU
    /// under it at InitLevel::CpuLocal + 1, long before InitLevel::Threading
    /// gives the boot CPU a thread whose preemption could be held off, and
    /// work_cpus() reads it at run time; IrqSave is the option that is right
    /// in both.
    sync::SpinLock m_lock;

    /// The affinity based on hardware/software.
    /// Using rules:  (m_hard_affinity & m_soft_affinity)
    ///
    /// Not under m_lock: both are written in init(), before the node is
    /// marked Online, and the hard one once more by join_hard_affinity() from
    /// the init hooks, on the boot CPU before any other CPU is started. They
    /// are only read after that. The accessors hand out references, which a
    /// lock could not cover; the one reader outside tests, the per-CPU unit
    /// mapper (CpuLocal::Mapper), reads the hard mask once, on the boot CPU,
    /// while CpuLocal::init() lays out the units. They used to be
    /// TA_GUARDED(get_lock()), which no reader honoured and no reader could.
    CpuMask m_hard_affinity;
    CpuMask m_soft_affinity;

    /// Which CPUs work on this node. Unlike the two above this one does
    /// change after bring-up -- CpuNodeMap::move() moves a CPU between
    /// nodes -- so it stays under the lock, and work_cpus() copies it out.
    CpuMask m_work_cpus TA_GUARDED(get_lock());

    Pfn m_start_pfn;

    std::atomic<usize> m_spanned_frames;

    ZoneList m_zone_list;
    PageQueues m_page_queues;

    /// The zones of this node an allocation has marked short since the
    /// reclaimer last took the record, one bit per zone type. One word, so
    /// that one exchange takes the whole record and one fetch_or tells a
    /// marker whether it is the first; those two atomic read-modify-writes
    /// are the only writes it has.
    std::atomic<u32> m_short_zones;

    [[no_unique_address]]
    AllocPolicy m_node_policy;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_NODE_HPP
