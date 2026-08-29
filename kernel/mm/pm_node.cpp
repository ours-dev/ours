#include <gktl/counter.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memory_model.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/panic.hpp>
#include <ours/task/thread.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/mem/object.hpp>
#include <ustl/util/enum_sequence.hpp>

#include <algorithm>
namespace ours::mm {
namespace {
    GKTL_COUNTER(pm_node_local_hit, "pm.node.local.hit");
    GKTL_COUNTER(pm_node_local_miss, "pm.node.local.miss");

    INIT_DATA Pfn g_zone_lower_pfn[NR_ZONES_PER_NODE];

    INIT_DATA Pfn g_zone_upper_pfn[NR_ZONES_PER_NODE];

    FORCE_INLINE auto clamp_pfn_to_zone(Pfn pfn, ZoneType ztype) -> Pfn {
        return std::clamp(pfn, g_zone_lower_pfn[static_cast<usize>(ztype)],
                          g_zone_upper_pfn[static_cast<usize>(ztype)]);
    }

    FORCE_INLINE auto gaf_to_allocation_control(Gaf gaf) -> AllocationControl {
        return AllocationControl();
    }

} // namespace

auto ZoneQueues::init_queues() -> void {
    ZoneQueue helper = queue_storage_;
    queues_[LocalSequential] = helper.subspan(0 * NR_ZONES_PER_NODE, NR_ZONES_PER_NODE);
    queues_[LocalContiguous] = helper.subspan(1 * NR_ZONES_PER_NODE, NR_ZONES_PER_NODE);
    queues_[SharedAffinity] = helper.subspan(2 * NR_ZONES_PER_NODE, MAX_ZONES);
    queues_[SharedSequential] = helper.subspan(2 * NR_ZONES_PER_NODE + MAX_ZONES, MAX_ZONES);
}

template <>
FORCE_INLINE auto ZoneQueues::insert_remote_queue<ZoneQueues::QueueType::SharedAffinity>(
    ZoneQueue const &remote_queue) -> void {
    for (auto &i: remote_queue) {
        push_back(i, QueueType::SharedAffinity);
    }
}

template <>
FORCE_INLINE auto
ZoneQueues::insert_remote_queue<ZoneQueues::QueueType::SharedSequential>(ZoneQueue const &queue)
    -> void {}

// Automatically dispatches insertion logic among different queues.
FORCE_INLINE auto ZoneQueues::insert_remote_queue(ZoneQueue const &queue) -> void {
    [&]<QueueType... Types>(ustl::EnumSequence<QueueType, Types...>) {
        (insert_remote_queue<Types>(queue), ...);
    }(ustl::MakeEnumSequenceT<QueueType, LocalContiguous + 1, MaxNumQueues>());
}

auto ZoneQueues::connect(NodeMask const &nodemask) -> void {
    NodeRoute node_path;
    PmNode::build_optimal_node_route(nid_, node_path, connected_);

    for (auto next: node_path) {
        auto zq = PmNode::node(next)->zone_queues();
        insert_remote_queue(zq->get_queue(QueueType::LocalContiguous));
    }
}

template <typename F>
auto ZoneQueues::for_each_zone(F &&f) -> ustl::traits::InvokeResultT<F, PmZone &> {}

template <typename F, typename Predicate>
auto ZoneQueues::for_each_zone_if(F &&f, Predicate &&p)
    -> ustl::traits::InvokeResultT<F, PmZone &> {}

static auto to_string(ZoneQueues::QueueType type) -> char const * {
    switch (type) {
        case ZoneQueues::LocalSequential:
            return "l-seq";
        case ZoneQueues::LocalContiguous:
            return "l-contig";
        case ZoneQueues::SharedAffinity:
            return "s-affi";
        case ZoneQueues::SharedSequential:
            return "s-seq";
        case ZoneQueues::MaxNumQueues:
            break; // Sentinel, not a queue.
    }

    return "Unknown";
}

auto ZoneQueues::dump_queue(QueueType type) const -> void {
    log::debug("Node[{}-{}]", nid_, to_string(type));

    auto const queue = get_queue(type);
    for (auto zone: queue) {
        if (zone) {
            log::debug("    None-{}| ZoneType-{:<6}| [0x{:X}, 0x{:X})", zone->which_node(),
                       to_string(zone->zone_type()), zone->start_pfn(), zone->end_pfn());
        } else {
            log::debug("    NULL");
        }
    }
}

struct ZoneIterator {
    typedef ZoneIterator Self;
    typedef ZoneQueues::ZoneRef ZoneRef;
    typedef ZoneQueues::ZoneQueue::iterator Inner;

    auto set_nodes(NodeMask const &nm) -> Self & {
        nodes = &nm;
        return *this;
    }

    auto set_zone_range(ZoneType lower, ZoneType upper) -> Self & {
        lower_zone_type = lower;
        upper_zone_type = upper;
        return *this;
    }

    auto set_zone_iterator(Inner begin, Inner end) -> Self & {
        first = begin;
        last = end;
        return *this;
    }

    auto move_next() -> ZoneRef;

    Inner first;
    Inner last;
    NodeMask const *nodes;
    ZoneType lower_zone_type; // Reserved and not used now.
    ZoneType upper_zone_type;
};

auto ZoneIterator::move_next() -> ZoneRef {
    while (first != last) {
        auto zref = (*first);
        if (zref->zone_type() > upper_zone_type) {
            ++first;
        }

        if (!nodes->test(zref->which_node())) {
            ++first;
        }

        // Advance for next iteration.
        return *(first++);
    }

    return ZoneRef();
}

decltype(PmNode::g_states) PmNode::g_states;
decltype(PmNode::g_nodes) PmNode::g_nodes;
decltype(PmNode::g_num_nodes) PmNode::g_num_nodes;
decltype(PmNode::g_num_online_nodes) PmNode::g_num_online_nodes;
decltype(PmNode::g_node_distance) PmNode::g_node_distance;
decltype(PmNode::g_allowed_gaf) PmNode::g_allowed_gaf = GAF_BOOT;

PmNode::PmNode(NnId nid)
    : m_canary()
    , m_id(nid)
    , m_lock()
    , m_hard_affinity()
    , m_soft_affinity()
    , m_start_pfn()
    , m_zone_queues(nid)
    , m_page_queues() {}

auto PmNode::bootstrap(ustl::views::Span<Pfn> max_zone_pfn) -> Status {
    ASSERT(!max_zone_pfn.empty());

    // Records all nodes that own a usable memory range.
    auto const visit_status =
        EarlyMem::visit_usable(EarlyMem::min_address(), EarlyMem::max_address(), bootmem::ANY_NODE,
                               [](bootmem::MemoryRegion const region) {
                                   PmNode::set_state(region.node, State::Possible);
                                   return true;
                               });
    if (visit_status != Status::Ok) {
        return visit_status;
    }

    auto start_pfn = phys_to_pfn(EarlyMem::min_address());
    for (usize i = 0; i < std::size(g_zone_lower_pfn); i++) {
        auto end_pfn = std::max(max_zone_pfn[i], start_pfn);
        g_zone_lower_pfn[i] = start_pfn;
        g_zone_upper_pfn[i] = end_pfn;
        start_pfn = end_pfn;
    }

    auto const &nodes = node_possible_mask();

    // Create all nodes.
    nodes.for_each([](NnId nid) {});

    // Initialize all nodes.
    nodes.for_each([](NnId nid) {
        auto const status = PmNode::node(nid)->init();
        ASSERT(status == Status::Ok);
    });

    // Connect all nodes.
    nodes.for_each([&](NnId nid) {
        auto zq = PmNode::node(nid)->zone_queues();
        zq->connect(nodes);
    });

    // Debug
    nodes.for_each([&](NnId nid) {
        auto zq = PmNode::node(nid)->zone_queues();
        zq->dump_queue(ZoneQueues::QueueType::SharedAffinity);
    });

    return Status::Ok;
}

auto PmNode::init() -> Status {
    m_canary.verify<Self>();

    auto const [node_start_pfn, node_end_pfn] = EarlyMem::get_pfn_range_node(m_id);
    m_start_pfn = node_start_pfn;
    m_spanned_frames = node_end_pfn - node_start_pfn;

    if (m_spanned_frames != 0) {
        set_state(m_id, State::Memory);
    }
    set_state(m_id, State::Online);

    auto const &cpu_to_node = Current::g_static_cpu_to_node;
    for (CpuNum cpu = 0; cpu < MAX_CPU; ++cpu) {
        if (cpu_to_node[cpu] == m_id) {
            m_hard_affinity.set(cpu);
        }
    }

    usize total_present_frames = 0;
    for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
        auto const ztype = ZoneType(i);
        auto const zone_start_pfn = clamp_pfn_to_zone(node_start_pfn, ztype);
        auto const zone_end_pfn = clamp_pfn_to_zone(node_end_pfn, ztype);

        if (zone_start_pfn < node_start_pfn || zone_end_pfn > node_end_pfn ||
            zone_end_pfn <= zone_start_pfn) {
            continue;
        }

        auto zone = EarlyMem::allocate<PmZone>(1, m_id);
        if (!zone) {
            panic("No enough memory to place node[{}].zone[{}]", m_id, to_string(ztype));
        }

        auto const num_present_frames =
            EarlyMem::count_present_frames(zone_start_pfn, zone_end_pfn);
        ASSERT(num_present_frames > 1);

        new (zone) PmZone(m_id, ztype, zone_start_pfn, zone_end_pfn, num_present_frames);

        zone_queues()->insert_local_zone(zone, ztype);

        total_present_frames += num_present_frames;
        PmNode::set_state(m_id, State(ztype));
    }

    m_present_frames = total_present_frames;

    DEBUG_ASSERT(!g_nodes[m_id]);
    g_nodes[m_id] = this;
    g_num_nodes += 1;
    return Status::Ok;
}

auto PmNode::build_optimal_node_route(NnId from, NodeRoute &path, NodeMask const &filter) -> void {
    NodeMask visited;
    visited.set(from, true);

    auto find_next = [&]() {
        NnId result = NUMA_MAX_NODE;
        usize distance = ustl::NumericLimits<usize>::max();
        for_each_nid(State::Memory, [&](NnId to) {
            if (visited.test(to) || filter.test(to)) {
                return;
            }

            auto const this_dis = PmNode::distance(from, to);
            if (distance > this_dis) {
                result = to;
                distance = this_dis;
            }
        });

        if (result != NUMA_MAX_NODE) {
            visited.set(result);
        }

        return result;
    };

    while (1) {
        auto next = find_next();
        if (next == NUMA_MAX_NODE) {
            break;
        }

        path.set_next(next);
    }
}

/// CPU's and Thread's capacity on nodes.
///
/// 1. The former storaged in PmNode::Current is an optimized occasion.
/// 2. The latter storaged in MemoryAllocationState is a hard constrict.

struct AllocationContext {
    auto build(PmNode *node, Gaf gaf, usize order, NodeMask const &nodemask) -> Status {
        ziter.set_nodes(nodemask).set_zone_range(gaf_zone_type(gaf), gaf_zone_type(gaf));

        ZoneQueues::QueueType queue_type;
        if (!!(gaf & Gaf::ThisNode)) {
            queue_type = ZoneQueues::LocalContiguous;
        } else {
            queue_type = ZoneQueues::SharedAffinity;
        }
        auto zq = node->zone_queues()->get_queue(queue_type);
        ziter.set_zone_iterator(zq.begin(), zq.end());

        return Status::Ok;
    }

    ZoneIterator ziter;

    NodeMask m_nodes;
};

auto PmNode::get_frame_from_zone_queues(Gaf gaf, usize order, AllocationControl control,
                                        MemoryAllocationContext const &context) -> PmFrame * {
    [[maybe_unused]]
    auto const no_fallback = !!(control & AllocationControl::NoFragment);
    m_zone_queues.for_each_zone([](PmZone &zone) {
        if (zone.is_below_high_wmark()) {
        }
    });

    // TODO(SmallHuaZi): the scan above is still a stub -- it inspects the
    // watermark but never takes a frame, so this always reports failure.
    return nullptr;
}

auto PmNode::alloc_frames_direct_reclaim(Gaf gaf, usize order, AllocationControl control,
                                         MemoryAllocationContext const &context) -> PmFrame * {
    return 0;
}

auto PmNode::alloc_frames_direct_compact(Gaf gaf, usize order, AllocationControl control,
                                         MemoryAllocationContext const &context) -> PmFrame * {
    return 0;
}

auto PmNode::alloc_frames_slowpath(Gaf gaf, usize order, MemoryAllocationContext const &context)
    -> PmFrame * {
    auto const never_fail = !!(gaf & Gaf::NoFail);
    if (never_fail) [[unlikely]] {
    }
    auto const control = gaf_to_allocation_control(gaf);
    auto const can_direct_reclaim = !!(gaf & Gaf::DirectReclaim);

    PmFrame *frame;
    while (1) {
        frame = get_frame_from_zone_queues(gaf, order, control, context);
        if (frame) {
            break;
        }

        if (!can_direct_reclaim) {
            break;
        }

        frame = alloc_frames_direct_reclaim(gaf, order, control, context);
        if (frame) {
            break;
        }

        frame = alloc_frames_direct_compact(gaf, order, control, context);
        if (frame) {
            break;
        }

        if (!!(gaf & Gaf::NoRetry)) {
            break;
        }

        if (never_fail) [[unlikely]] {
            if (!can_direct_reclaim) {
                break;
            }

            task::Thread::Current::reschedule();
        }
    }

    return frame;
}

auto PmNode::alloc_frames_preamble(Gaf, usize, MemoryAllocationContext *) -> Status {
    // TODO(SmallHuaZi): unimplemented -- should prepare the allocation context
    // (watermarks, fallback nodemask) before the zone walk.
    return Status::Ok;
}

auto PmNode::alloc_frames_postamble(PmFrame *frame, Gaf gaf, usize order,
                                    MemoryAllocationContext const &context) -> void {
    if (PmNode::Current::nid() == m_id) {
        pm_node_local_hit.add(1);
    } else {
        pm_node_local_miss.add(1);
    }
}

auto PmNode::alloc_frames(Gaf gaf, usize order, NodeMask const &fallback)
    -> ktl::Result<PmFrame *> {
    m_canary.verify<Self>();

    if (order > MAX_FRAME_ORDER) {
        return ustl::err(Status::InvalidArguments);
    }
    gaf &= g_allowed_gaf;

    MemoryAllocationContext context;
    if (auto const status = alloc_frames_preamble(gaf, order, &context); Status::Ok != status) {
        return ustl::err(status);
    }
    auto const control = gaf_to_allocation_control(gaf);

    // First attempt to allocate.
    auto frame = get_frame_from_zone_queues(gaf, order, control, context);
    if (!frame) {
        frame = alloc_frames_slowpath(gaf, order, context);
    }

    alloc_frames_postamble(frame, gaf, order, context);
    return ustl::ok(frame);
}

auto PmNode::alloc_frames_fragment(Gaf gaf, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {

    PmFrame::List list;
    for (usize order = 0; n > 0; order += 1) {
        if ((n & 1) == 0) {
            continue;
        }

        if (auto frame = this->alloc_frames(gaf, order, fallback)) {
            list.push_back(*frame.unwrap());
            n >>= 1;
        } else {
            free_frames(&list);
            return ustl::err(frame.unwrap_err());
        }
    }

    return ustl::ok(ustl::move(list));
}

auto PmNode::alloc_frames_bulk(Gaf flags, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {

    PmFrame::List list;
    for (usize i = 0; i < n; ++i) {
        if (auto frame = this->alloc_frames(flags, 0, fallback)) {
            list.push_back(*frame.unwrap());
        } else {
            free_frames(&list);
            return ustl::err(frame.unwrap_err());
        }
    }

    return ustl::ok(ustl::move(list));
}

auto PmNode::free_frames(PmFrame *frame, usize order) -> void {
    DEBUG_ASSERT(frame, "");
    auto const node = PmNode::node(frame->nid());
    auto const zone = node->m_zone_queues.get_local_zone(frame->zone());
    DEBUG_ASSERT(zone, "");

    zone->free_frame(frame, order);
}

auto PmNode::free_frames(PmFrame::List *list) -> void {
    list->clear_and_dispose([](PmFrame *frame) noexcept { free_frames(frame, frame->order()); });
}

auto PmNode::dump() const -> void {
    // TODO(SmallHuaZi) merge them to a log sentence.
    log::info(
        R"(Node[{}]: 
                start pfn: {}
                spanned frames: {}
                present frames: {}
                reserved frames: {}
            )",
        m_id, m_start_pfn, m_spanned_frames.load(), m_present_frames.load(),
        m_reserved_frames.load());

    for (auto zone: m_zone_queues.get_queue(ZoneQueues::LocalContiguous)) {
        log::info(R"(ZoneType[{}]: 
                    start pfn: {}
                    spanned frames: {}
                    present frames: {}
                    reserved frames: {}
                )",
                  to_string(zone->zone_type()), zone->start_pfn(), zone->spanned_frames(),
                  zone->present_frames(), zone->reserved_frames());
    }
}

constinit decltype(PmNode::Current::g_static_cpu_to_node) PmNode::Current::g_static_cpu_to_node =
    [] {
        decltype(PmNode::Current::g_static_cpu_to_node) value;
        std::fill(std::begin(value), std::end(value), NUMA_MAX_NODE);
        return value;
    }();

constinit decltype(PmNode::Current::g_dynamic_cpu_to_node) PmNode::Current::g_dynamic_cpu_to_node =
    [] {
        decltype(PmNode::Current::g_dynamic_cpu_to_node) value;
        std::fill(std::begin(value), std::end(value), NUMA_MAX_NODE);
        return value;
    }();

auto PmNode::Current::inherit_thread_context(Gaf &gaf) -> Status {
    auto &mem_state = task::Thread::Current::memory_allocation_state();
    if (!mem_state.is_enabled()) {
        return Status::AccessDenied;
    }

    if constexpr (MEMORY_ALLOCATION_POLICY) {
        using Kind = MemoryAllocationPolicy::Kind;
        if (auto policy = mem_state.get_policy()) {
            switch (policy->kind()) {
                case Kind::Bind:
                    break;
                case Kind::Interleave:
                    break;
                case Kind::Preferred:
                    break;
                default:
                    break;
            }
        }
    }

    return Status::Ok;
}

auto PmNode::Current::alloc_frames(Gaf gaf, usize order, NodeMask const &fallback)
    -> ktl::Result<PmFrame *> {
    if (auto const status = inherit_thread_context(gaf); Status::Ok != status) {
        return ustl::err(status);
    }

    auto const local_node = PmNode::Current::get();
    DEBUG_ASSERT(local_node);
    local_node->m_canary.verify<Self>();

    auto const current_thread = task::Thread::Current::get();
    Guard guard{current_thread->get_resource_lock()};

    return local_node->alloc_frames(gaf, order, fallback);
}

auto PmNode::Current::alloc_frames_fragment(Gaf gaf, usize order, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    if (auto const status = inherit_thread_context(gaf); Status::Ok != status) {
        return ustl::err(status);
    }

    auto const local_node = PmNode::Current::get();
    DEBUG_ASSERT(local_node);
    local_node->m_canary.verify<Self>();

    auto const current_thread = task::Thread::Current::get();
    Guard guard{current_thread->get_resource_lock()};

    return local_node->alloc_frames_fragment(gaf, order, fallback);
}

auto PmNode::Current::alloc_frames_bulk(Gaf gaf, usize order, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    if (auto const status = inherit_thread_context(gaf); Status::Ok != status) {
        return ustl::err(status);
    }

    auto const local_node = PmNode::Current::get();
    DEBUG_ASSERT(local_node);
    local_node->m_canary.verify<Self>();

    auto const current_thread = task::Thread::Current::get();
    Guard guard{current_thread->get_resource_lock()};

    return local_node->alloc_frames_bulk(gaf, order, fallback);
}

auto PmNode::Current::set_nid(CpuNum cpunum, NnId new_nid) -> void {
    DEBUG_ASSERT(cpunum < MAX_CPU);
    DEBUG_ASSERT(new_nid < NUMA_MAX_NODE);

    auto const old_nid = g_dynamic_cpu_to_node[cpunum];
    if (old_nid == new_nid) {
        return;
    }

    if (old_nid != NUMA_MAX_NODE) {
        if (auto node = PmNode::node(old_nid)) {
            node->m_canary.verify<Self>();
            Guard guard{node->get_lock()};

            // Erases ourselves from the node to which the CPU belongs.
            node->m_work_cpus.set(cpunum, 0);
        }
    }

    g_dynamic_cpu_to_node[cpunum] = new_nid;
    if (auto node = PmNode::node(new_nid)) {
        node->m_canary.verify<Self>();
        Guard guard{node->get_lock()};

        node->m_work_cpus.set(cpunum);
        PmNode::Current::set(node, cpunum);
    }
}

auto PmNode::Current::set_nid(NnId nid) -> void {
    PmNode::Current::set_nid(Cpu::id(), nid);
}

auto PmNode::Current::init() -> void {
    Cpu::possible_mask().for_each(
        [](CpuNum cpu) { PmNode::Current::set_nid(cpu, g_static_cpu_to_node[cpu]); });
}

// When cpu local area has been setup, we should install local node id to enable the version of
// `alloc_frames` without specifing explicitly `nid`.
GKTL_INIT_HOOK(RegisterNode, PmNode::Current::init, gktl::InitLevel::CpuLocal + 1);

} // namespace ours::mm
