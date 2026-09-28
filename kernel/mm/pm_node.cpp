#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/panic.hpp>
#include <ours/task/thread.hpp>

#include <algorithm>

#include <gktl/counter.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/mem/object.hpp>
#include <ustl/util/enum_sequence.hpp>
namespace ours::mm {
namespace {
    GKTL_COUNTER(PM_NODE_LOCAL_HIT, "pm.node.local.hit");
    GKTL_COUNTER(PM_NODE_LOCAL_MISS, "pm.node.local.miss");

    FORCE_INLINE
    auto gaf_to_allocation_control(Gaf gaf) -> AllocationOptions {
        return AllocationOptions();
    }

} // namespace

/// Chains the zones of every node in |nodemask| behind this node's own, nearest
/// node first.
///
/// |nodemask| bounds the route; m_connected records which nodes the route
/// actually chained in. Each remote node contributes its own zones in type
/// order -- the list LocalContiguous used to keep a second copy of.
auto ZoneList::connect(NodeMask const &nodemask) -> void {
    auto const route = PmNode::Router::build(m_nid, nodemask);
    for (auto const next: route) {
        PmNode::node(next)->zone_list()->for_each_local_zone(
            [this](PmZone &zone) { push_fallback(&zone); });
        m_connected.set(next);
    }
}

/// Visits the zones in allocation order: the fallback list already *is* that
/// order, this node's own zones first and the remote ones in the order
/// connect() computed.
///
/// A callback returning void visits every zone. One returning a value stops at
/// the first result that tests true and yields it -- which is what makes this
/// usable as "take a frame from the first zone that can spare one" rather than
/// only as a traversal.
template <typename F>
auto ZoneList::for_each_zone(F &&f) -> ustl::traits::InvokeResultT<F, PmZone &> {
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

template <typename F, typename Predicate>
auto ZoneList::for_each_zone_if(F &&f, Predicate &&p) -> ustl::traits::InvokeResultT<F, PmZone &> {
    return for_each_zone([&](PmZone &zone) { return p(zone) ? f(zone) : decltype(f(zone)){}; });
}

auto ZoneList::dump_fallback() const -> void {
    log::debug("Node[{}-fallback]", m_nid);
    for (auto const *const zone: m_fallback) {
        log::debug("    None-{}| ZoneType-{:<6}| [0x{:X}, 0x{:X})", zone->which_node(),
                   to_string(zone->zone_type()), zone->start_pfn(), zone->end_pfn());
    }
}

decltype(PmNode::STATES) PmNode::STATES;
decltype(PmNode::NODES) PmNode::NODES;
decltype(PmNode::NUM_NODES) PmNode::NUM_NODES;
decltype(PmNode::ALLOWED_GAF) PmNode::ALLOWED_GAF = GAF_BOOT;

PmNode::PmNode(NnId nid)
    : m_canary(this)
    , m_id(nid)
    , m_lock()
    , m_hard_affinity()
    , m_soft_affinity()
    , m_start_pfn()
    , m_zone_list(nid)
    , m_page_queues() {}

auto PmNode::Router::build(NnId const from, NodeMask const &candidates) -> Router {
    Router route;
    NodeMask visited;
    visited.set(from, true);

    auto find_next = [&]() {
        NnId result = NUMA_MAX_NODE;
        usize distance = ustl::NumericLimits<usize>::max();
        for_each_nid(State::Memory, [&](NnId to) {
            if (visited.test(to) || !candidates.test(to)) {
                return;
            }

            auto const this_dis = Topology::distance(from, to);
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

        route.push_back(next);
    }
    return route;
}

auto PmNode::get_frame_from_zone_list(Gaf gaf, usize order, AllocationOptions options,
                                      MemoryAllocationContext const &context) -> PmFrame * {
    // TODO(SmallHuaZi): AllocationOptions::NoFragment is not honoured yet. It
    // should stop the walk before the remote zones rather than being ignored,
    // but what exactly it forbids has not been settled.
    return m_zone_list.for_each_zone([&](PmZone &zone) -> PmFrame * {
        if (!context.allowed_nodes.test(zone.which_node())) {
            return nullptr;
        }
        return zone.alloc_frames(gaf, order, options, context);
    });
}

auto PmNode::alloc_frames_direct_reclaim(Gaf gaf, usize order, AllocationOptions options,
                                         MemoryAllocationContext const &context) -> PmFrame * {
    return 0;
}

auto PmNode::alloc_frames_direct_compact(Gaf gaf, usize order, AllocationOptions options,
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
        frame = get_frame_from_zone_list(gaf, order, control, context);
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

auto PmNode::alloc_frames_preamble(Gaf gaf, usize order, NodeMask const &fallback,
                                   MemoryAllocationContext *context) -> Status {
    DEBUG_ASSERT(context, "");

    // The nodes this allocation may draw from. Without this the zone walk in
    // get_frame_from_zone_list() rejects everything, since an unset NodeMask
    // answers false for every node.
    //
    // Gaf::ThisNode narrows it to the local node; it is a preference rather than
    // a restriction, so an empty intersection falls back to the caller's mask
    // rather than failing -- a node with no memory of its own must still be able
    // to allocate.
    context->allowed_nodes = fallback;
    if (!!(gaf & Gaf::ThisNode)) {
        auto local = fallback;
        local &= NodeMask::of(m_id);
        if (local.any()) {
            context->allowed_nodes = local;
        }
    }

    context->home_node = m_id;
    context->initiator = Cpu::id();

    // TODO(SmallHuaZi): watermarks. The zone walk consults is_below_high_wmark(),
    // and nothing sets that flag yet, so every zone currently reads as being
    // above its watermark.
    static_cast<void>(order);
    return Status::Ok;
}

auto PmNode::alloc_frames_postamble(PmFrame *frame, Gaf gaf, usize order,
                                    MemoryAllocationContext const &context) -> void {
    if (Topology::current_node() == m_id) {
        PM_NODE_LOCAL_HIT.add(1);
    } else {
        PM_NODE_LOCAL_MISS.add(1);
    }
}

auto PmNode::alloc_frames(Gaf gaf, usize order, NodeMask const &fallback)
    -> ktl::Result<PmFrame *> {
    m_canary.verify<Self>();

    // Whether the calling thread may allocate at all: a thread that has
    // disabled allocation (MemoryAllocationState::disable()) is refused here,
    // whichever way it asked. This is the one function every frame
    // allocation reaches -- the node-less entries through
    // PmNode::Current, the ones naming a node directly, and the bulk and
    // fragment loops once per step -- so the rule cannot be walked around.
    //
    // It used to be asked only by PmNode::Current::inherit_thread_context(),
    // bundled with the NUMA policy, so alloc_frames(nid, ...) skipped it along
    // with the policy it had every right to skip. The two are different
    // questions -- may this thread allocate, and where from -- and only the
    // second one depends on whether the caller named a node. Linux splits them
    // the same way: current_gfp_context() applies the task's scoped
    // restrictions in __alloc_pages(), which alloc_pages_node() reaches too,
    // while the mempolicy is consulted only by alloc_pages().
    //
    // Thread::Current is always there by now: task::init_early() makes the
    // boot thread current before the PMM exists.
    if (!task::Thread::Current::memory_allocation_state().is_enabled()) {
        return ustl::err(Status::AccessDenied);
    }

    if (order > MAX_FRAME_ORDER) {
        return ustl::err(Status::InvArgs);
    }
    gaf &= ALLOWED_GAF;

    MemoryAllocationContext context;
    if (auto const status = alloc_frames_preamble(gaf, order, fallback, &context);
        Status::Ok != status) {
        return ustl::err(status);
    }
    auto const control = gaf_to_allocation_control(gaf);

    // First attempt to allocate.
    auto frame = get_frame_from_zone_list(gaf, order, control, context);
    if (!frame) {
        frame = alloc_frames_slowpath(gaf, order, context);
    }

    alloc_frames_postamble(frame, gaf, order, context);
    return ustl::ok(frame);
}

auto PmNode::alloc_frames_fragment(Gaf gaf, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {

    PmFrame::List list;
    // One bit of |n| per step, whether or not it is set. The shift used to
    // sit inside the branch that allocates, so the first clear bit was never
    // shifted out: `continue` bumped the order and tested the same bit again,
    // for ever. Any even |n| hung here -- n = 2 on its first step.
    for (usize order = 0; n > 0; order += 1, n >>= 1) {
        if ((n & 1) == 0) {
            continue;
        }

        if (auto frame = this->alloc_frames(gaf, order, fallback)) {
            list.push_back(*frame.unwrap());
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
    auto const zone = node->m_zone_list.local_zone(frame->zone());
    DEBUG_ASSERT(zone, "");

    zone->free_frame(frame, order);
}

auto PmNode::free_frames(PmFrame::List *list) -> void {
    list->clear_and_dispose([](PmFrame *frame) noexcept { free_frames(frame, frame->order()); });
}

auto PmNode::contains(usize const order, ZoneType const type) -> bool {
    m_canary.verify<Self>();

    auto *const zone = m_zone_list.local_zone(type);
    if (!zone) {
        return false;
    }
    return zone->contains(order);
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

    m_zone_list.for_each_local_zone([](PmZone &zone) {
        log::info(R"(ZoneType[{}]: 
                    start pfn: {}
                    spanned frames: {}
                    present frames: {}
                    reserved frames: {}
                )",
                  to_string(zone.zone_type()), zone.start_pfn(), zone.spanned_frames(),
                  zone.present_frames(), zone.reserved_frames());
    });
}

auto PmNode::Current::apply_thread_policy(Gaf &gaf) -> Status {
    if constexpr (MEMORY_ALLOCATION_POLICY) {
        using Kind = MemoryAllocationPolicy::Kind;

        // The thread's resource lock covers reading the policy and nothing
        // more: it is what another thread takes to replace it, as Linux's
        // task_lock() guards task->mempolicy. It used to be held by every
        // node-less allocation entry around the whole allocation, slow path
        // included -- a spin lock with interrupts off, held across a path that
        // may reschedule() for Gaf::NoFail. Copying the Arc out is all that
        // needs it.
        auto const policy = [] {
            auto *const thread = task::Thread::Current::get();
            Guard<sync::SpinLock, IrqSave> guard{thread->get_resource_lock()};
            return thread->memory_allocation_state().get_policy();
        }();
        if (policy) {
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

auto PmNode::Current::resolve_local_node() -> PmNode * {
    // The fast answer, and the only one once the init hook has run.
    if (auto *const node = Current::get()) {
        return node;
    }

    auto const nid = Topology::current_node();
    if (nid >= NUMA_MAX_NODE) {
        // No topology source has claimed this CPU. bootstrap() claims the boot
        // CPU before the first allocation, so this is an AP asking before
        // claim_possible_cpus() has run -- a caller error, not a shortage.
        return nullptr;
    }
    return PmNode::node(nid);
}

/// What every node-less entry does before it can allocate: apply the thread's
/// memory policy to |gaf|, then find the calling CPU's node.
///
/// The three entries below used to repeat these steps -- and the comment on the
/// null check -- line for line, differing only in which PmNode method they
/// ended with. A fourth entry would have been a fourth copy, and a fix to one
/// of the steps a change in three places.
auto PmNode::Current::local_node_for(Gaf &gaf) -> ktl::Result<PmNode *> {
    if (auto const status = apply_thread_policy(gaf); Status::Ok != status) {
        return ustl::err(status);
    }

    auto *const local_node = Current::resolve_local_node();
    if (!local_node) {
        // A real return rather than DEBUG_ASSERT: this used to assert and then
        // dereference anyway, so a release build walked through a null PmNode
        // instead of failing here.
        return ustl::err(Status::BadState);
    }
    local_node->m_canary.verify<Self>();
    return ustl::ok(local_node);
}

auto PmNode::Current::alloc_frames(Gaf gaf, usize order, NodeMask const &fallback)
    -> ktl::Result<PmFrame *> {
    auto const node = local_node_for(gaf);
    if (!node) {
        return ustl::err(node.unwrap_err());
    }
    return (*node)->alloc_frames(gaf, order, fallback);
}

auto PmNode::Current::alloc_frames_fragment(Gaf gaf, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    auto const node = local_node_for(gaf);
    if (!node) {
        return ustl::err(node.unwrap_err());
    }
    return (*node)->alloc_frames_fragment(gaf, n, fallback);
}

auto PmNode::Current::alloc_frames_bulk(Gaf gaf, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    auto const node = local_node_for(gaf);
    if (!node) {
        return ustl::err(node.unwrap_err());
    }
    return (*node)->alloc_frames_bulk(gaf, n, fallback);
}

auto PmNode::Current::set_nid(CpuNum cpunum, NnId new_nid) -> void {
    DEBUG_ASSERT(cpunum < MAX_CPU);
    DEBUG_ASSERT(new_nid < NUMA_MAX_NODE);

    auto const old_nid = Topology::cpu_to_node(cpunum);
    if (old_nid == new_nid) {
        return;
    }

    if (old_nid != NUMA_MAX_NODE) {
        if (auto node = PmNode::node(old_nid)) {
            node->m_canary.verify<Self>();
            Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};

            // Erases ourselves from the node to which the CPU belongs.
            node->m_work_cpus.set(cpunum, 0);
        }
    }

    Topology::set_cpu_node(cpunum, new_nid);
    if (auto node = PmNode::node(new_nid)) {
        node->m_canary.verify<Self>();
        Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};

        node->m_work_cpus.set(cpunum);
        PmNode::Current::set(node, cpunum);
    }
}

auto PmNode::Current::set_nid(NnId nid) -> void {
    PmNode::Current::set_nid(Cpu::id(), nid);
}

/// Publishes each CPU's node into its per-CPU slot, now that the slots exist.
///
/// Not through set_nid(). bootstrap() has already claimed every CPU with
/// Topology::set_cpu_node_early(), which fills both tables, so set_nid() finds the old and
/// new ids equal and returns before it reaches the per-CPU pointer. That is
/// what left CURRENT_INSTANCE null across the zero-page allocation in
/// init_vmm_preheap(): Current::alloc_frames() went on through a null
/// PmNode, and get_frame_from_zone_list() spun on the ZoneList it read
/// out of address 0x48. The early return is right for what set_nid() is
/// for -- a CPU moving between nodes at run time, where nothing changing
/// means nothing to do -- but the first publish is this function's job, and
/// it has to be unconditional.
///
/// The running CPU is covered by name, the way bootstrap() covers it. The
/// possible mask is filled by the MADT parse, which runs from
/// platform_init_apic() at InitLevel::Irq -- three levels after this hook --
/// so at this point it is empty, and a walk of it alone publishes nothing.
/// That was the other way this used to fail on the same boot.
auto PmNode::Current::publish(CpuNum const cpu) -> void {
    auto const nid = Topology::cpu_to_node(cpu);
    if (nid == NUMA_MAX_NODE) {
        // Not claimed yet. bootstrap() claims only the CPU it runs on;
        // claim_possible_cpus() and the NUMA topology parse claim the rest.
        return;
    }

    auto *const node = PmNode::node(nid);
    DEBUG_ASSERT(node, "CPU {} was claimed by node {}, which does not exist", cpu, nid);
    node->m_canary.verify<Self>();

    Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};
    node->m_work_cpus.set(cpu);
    // PmNode::init() builds the hard affinity from Topology::firmware_node(), but
    // only for the CPUs claimed by then -- the boot CPU. A CPU claimed later
    // joins it here, so that the node's mask says the same thing as the table.
    if (Topology::firmware_node(cpu) == nid) {
        node->m_hard_affinity.set(cpu);
    }
    PmNode::Current::set(node, cpu);
}

auto PmNode::Current::init() -> void {
    Cpu::possible_mask().for_each(&Current::publish);
    publish(CpuLocal::current_cpu());
}

/// Every CPU the MADT reported gets a node -- before any of them runs.
///
/// bootstrap() could only claim the CPU it runs on: the possible mask is
/// filled by the MADT parse (platform_init_apic(), InitLevel::Irq), long
/// after the PMM. Every other CPU was left on the NUMA_MAX_NODE sentinel, and
/// nothing ever moved it -- the only other claimant is the SRAT parse, which
/// a kernel built without NUMA never compiles in and a machine without an
/// SRAT never feeds. So current_node() on an AP answered NUMA_MAX_NODE, and
/// the first kmalloc() on an AP -- a user thread's channel_create(), on
/// CPU1 -- read m_node_caches[NUMA_MAX_NODE], one past the end, took the
/// ListMemberHook after the array for a NodeCache, and spun for ever on a list
/// pointer it had mistaken for that NodeCache's lock. The comment on
/// claim_unplaced_cpus() in pm_node_init.cpp describes that exact failure for
/// the boot CPU; this is the same hole for the others.
///
/// A CPU no topology source placed goes where the boot CPU went, which is the
/// first node that owns memory -- the fallback bootstrap() chose, and on a
/// machine without an SRAT the only node there is. Linux does this in the same
/// place and for the same reason: init_cpu_to_node() after the ACPI parse,
/// with numa_set_node() for each possible CPU, before smp_init() brings the
/// APs up.
auto PmNode::Current::claim_possible_cpus() -> void {
    auto const fallback = Topology::current_node();
    DEBUG_ASSERT(fallback < NUMA_MAX_NODE, "the boot CPU has no node");

    Cpu::possible_mask().for_each([fallback](CpuNum const cpu) {
        if (Topology::cpu_to_node(cpu) == NUMA_MAX_NODE) {
            Topology::set_cpu_node_early(cpu, fallback);
        }
        publish(cpu);
    });
}

/// Once per-CPU storage exists, so that alloc_frames() can be called without
/// naming a node. CpuLocal + 1 rather than CpuLocal itself: the hooks at a
/// level run when that level is announced, and CpuLocal is announced right
/// after CpuLocal::init() returns -- a hook at exactly that level would run
/// with the units laid out but before main.cpp had said so.
GKTL_INIT_HOOK(RegisterNode, PmNode::Current::init, gktl::InitLevel::CpuLocal + 1);

/// After InitLevel::Irq, where the MADT parse fills the possible mask, and
/// before InitLevel::Platform, where x86_bring_aps() starts the CPUs in it.
GKTL_INIT_HOOK(ClaimPossibleCpus, PmNode::Current::claim_possible_cpus, gktl::InitLevel::Irq + 1);

} // namespace ours::mm
