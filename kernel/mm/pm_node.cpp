#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/cpu_node_map.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/panic.hpp>
#include <ours/task/thread.hpp>

#include <algorithm>

#include <gktl/counter.hpp>
#include <gktl/defer.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <numa/topology.hpp>
#include <ustl/function/bind.hpp>
#include <ustl/mem/object.hpp>
#include <ustl/util/enum_sequence.hpp>
namespace ours::mm {
namespace {
    GKTL_COUNTER(PM_NODE_LOCAL_HIT, "pm.node.local.hit");
    GKTL_COUNTER(PM_NODE_LOCAL_MISS, "pm.node.local.miss");

    auto apply_thread_policy(Gaf &alloc_flags) -> Status {
        if constexpr (MEMORY_ALLOCATION_POLICY) {
            if (!!(alloc_flags & Gaf::ThisNode)) {
                return Status::Ok;
            }
            using Kind = AllocPolicy::Kind;

            // The thread's resource lock covers reading the policy and nothing
            // more: it is what another thread takes to replace it. It used to be
            // held by every node-less allocation entry around the whole
            // allocation, slow path included -- a spin lock with interrupts off,
            // held across a path that could reschedule(). Copying the Arc out is
            // all that needs it.
            auto const policy = [] {
                auto *const thread = task::Thread::Current::get();
                Guard<sync::SpinLock, IrqSave> guard{thread->get_resource_lock()};
                return thread->alloc_state().policy();
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

    auto resolve_home(NodeId nid, Gaf &alloc_flags) -> ktl::Result<PmNode *> {
        if (nid < MAX_NODES) {
            if (auto *const node = PmNode::node(nid)) {
                return ustl::ok(node);
            }
            return ustl::err(Status::InvArgs);
        }

        if (auto const status = apply_thread_policy(alloc_flags); Status::Ok != status) {
            return ustl::err(status);
        }
        auto *const node = CpuNodeMap::local_node();
        if (!node) {
            return ustl::err(Status::BadState);
        }
        return ustl::ok(node);
    }

} // namespace

/// Chains the zones of every node in |order| behind this node's own, in the
/// order given.
///
/// m_connected records which nodes were chained in. Each remote node
/// contributes its own zones in type order -- the list LocalContiguous used to
/// keep a second copy of.
///
/// It used to take a NodeMask and call Router::build() to turn it into an
/// order, which meant this function read the machine's distance matrix. The
/// derivation is numa::build_fallback_orders(), asked once by bring-up; what
/// arrives here is already a list of node ids, and the only thing left to
/// decide is nothing.
auto ZoneList::connect(ustl::views::Span<NodeId const> const order) -> void {
    for (auto const next: order) {
        DEBUG_ASSERT(next != m_nid, "node {} was given itself as a fallback hop", m_nid);
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

PmNode::PmNode(NodeId nid)
    : m_canary(this)
    , m_id(nid)
    , m_lock()
    , m_hard_affinity()
    , m_soft_affinity()
    , m_start_pfn()
    , m_zone_list(nid)
    , m_page_queues() {}

auto PmNode::alloc_frames(Gaf alloc_flags, usize order, NodeMask const &fallback)
    -> ktl::Result<PmFrame *> {
    return alloc_frames(NodeId{MAX_NODES}, alloc_flags, order, fallback);
}

auto PmNode::alloc_frames(NodeId const nid, Gaf alloc_flags, usize order, NodeMask const &fallback)
    -> ktl::Result<PmFrame *> {
    auto const home = resolve_home(nid, alloc_flags);
    if (!home) {
        observe_pressure();
        return ustl::err(home.unwrap_err());
    }
    auto const result = (*home)->take_block(alloc_flags, order, fallback);
    if (!result) {
        auto const &failure = result.error();
        observe_pressure(failure.depleted_zone);
        return ustl::err(failure.status);
    }
    observe_pressure();
    return ustl::ok(*result);
}

auto PmNode::take_block(Gaf alloc_flags, usize order, NodeMask const &fallback)
    -> ustl::Result<PmFrame *, BlockFailure> {
    m_canary.verify<Self>();

    // Whether the calling thread may allocate at all: a thread that has
    // disabled allocation (AllocState::disable()) is refused here,
    // whichever way it asked. This is the one function every frame
    // allocation reaches -- the node-less entries in pmm.cpp, the ones naming
    // a node directly, and the bulk and fragment loops once per step -- so the
    // rule cannot be walked around.
    //
    // It used to be asked only by inherit_thread_context() on the per-CPU
    // helper class that has since been dissolved,
    // bundled with the NUMA policy, so alloc_frames(nid, ...) skipped it along
    // with the policy it had every right to skip. The two are different
    // questions -- may this thread allocate, and where from -- and only the
    // second one depends on whether the caller named a node. So this one is
    // asked on every path, node named or not, and the policy only on the
    // node-less ones.
    //
    // Thread::Current is always there by now: task::init_early() makes the
    // boot thread current before the PMM exists.
    if (!task::Thread::Current::alloc_state().is_enabled()) {
        return ustl::err(BlockFailure{.status = Status::AccessDenied});
    }

    if (order > MAX_FRAME_ORDER) {
        return ustl::err(BlockFailure{.status = Status::InvArgs});
    }

    auto const initiator = Cpu::id();
    auto const initiator_node = CpuNodeMap::node_of(initiator);
    auto const request = resolve_request(alloc_flags, order, fallback,
                                         AllocRequest::Origin{.node = m_id, .cpu = initiator});

    ustl::Option<PmZone const &> depleted_zone;
    for (auto &zone: m_zone_list.candidates(request)) {
        auto const block = zone.take_block(request.order());
        if (!block) {
            if (order == 0 && !depleted_zone &&
                zone.num_free_frames() < zone.watermark(PmZone::Watermark::High)) {
                depleted_zone = ustl::some_ref(zone);
            }
            continue;
        }

        auto *const frame = &block.value();
        if (!!(request.flags() & Gaf::Zero)) {
            std::fill_n(frame_to_virt<isize>(frame),
                        (usize{1} << request.order()) * PAGE_SIZE / sizeof(isize), 0);
        }
        if (frame->nid() == initiator_node) {
            PM_NODE_LOCAL_HIT.add(1);
        } else {
            PM_NODE_LOCAL_MISS.add(1);
        }
        return ustl::ok(frame);
    }

    return ustl::err(BlockFailure{.status = Status::OutOfMem, .depleted_zone = depleted_zone});
}

auto PmNode::alloc_frames_fragment(Gaf alloc_flags, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    return alloc_frames_fragment(NodeId{MAX_NODES}, alloc_flags, n, fallback);
}

auto PmNode::alloc_frames_fragment(NodeId const nid, Gaf alloc_flags, usize n,
                                   NodeMask const &fallback) -> ktl::Result<PmFrame::List> {
    auto observation = gktl::defer([] { observe_pressure(); });
    auto const home = resolve_home(nid, alloc_flags);
    if (!home) {
        return ustl::err(home.unwrap_err());
    }
    PmFrame::List list;
    auto rollback = gktl::defer([&list] { return_blocks(list); });
    // One bit of |n| per step, whether or not it is set. The shift used to
    // sit inside the branch that allocates, so the first clear bit was never
    // shifted out: `continue` bumped the order and tested the same bit again,
    // for ever. Any even |n| hung here -- n = 2 on its first step.
    for (usize order = 0; n > 0; order += 1, n >>= 1) {
        if ((n & 1) == 0) {
            continue;
        }

        if (auto frame = (*home)->take_block(alloc_flags, order, fallback)) {
            list.push_back(*frame.unwrap());
        } else {
            return ustl::err(frame.error().status);
        }
    }

    rollback.cancel();
    return ustl::ok(ustl::move(list));
}

auto PmNode::alloc_frames_bulk(Gaf alloc_flags, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    return alloc_frames_bulk(NodeId{MAX_NODES}, alloc_flags, n, fallback);
}

auto PmNode::alloc_frames_bulk(NodeId const nid, Gaf alloc_flags, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    auto observation = gktl::defer([] { observe_pressure(); });
    auto const home = resolve_home(nid, alloc_flags);
    if (!home) {
        return ustl::err(home.unwrap_err());
    }
    PmFrame::List list;
    auto rollback = gktl::defer([&list] { return_blocks(list); });
    for (usize i = 0; i < n; ++i) {
        if (auto frame = (*home)->take_block(alloc_flags, 0, fallback)) {
            list.push_back(*frame.unwrap());
        } else {
            return ustl::err(frame.error().status);
        }
    }

    rollback.cancel();
    return ustl::ok(ustl::move(list));
}

auto PmNode::return_block(PmFrame &frame, usize order) -> void {
    // The runtime half of the guard in early_mem.cpp's free_frames_phys_range():
    // a reserved frame reaches the buddy only through unwire_frame(). Nothing
    // else on this path looks at the frame, so without this a reserved frame
    // would be handed out again still flagged and still counted.
    DEBUG_ASSERT(!(frame.flags() & page::Flags::Reserved),
                 "frame {:#x} is reserved; unwire_frame() it before freeing",
                 frame_to_phys(&frame));
    DEBUG_ASSERT(!frame.is_role(page::Role::Vmm) || 0 == frame.page_queue(),
                 "frame {:#x} is freed while still on a page queue", frame_to_phys(&frame));
    // The frame says its node and its type, and the table has the zone; the
    // node itself is not needed.
    auto *const zone = PmZone::at(frame.nid(), frame.zone());
    DEBUG_ASSERT(zone, "frame {:#x} is in no zone", frame_to_phys(&frame));

    zone->return_block(frame, order);
}

auto PmNode::return_blocks(PmFrame::List &list) -> void {
    list.clear_and_dispose([](PmFrame *frame) noexcept { return_block(*frame, frame->order()); });
}

auto PmNode::free_frames(PmFrame *frame, usize order) -> void {
    DEBUG_ASSERT(frame, "free requires a frame");
    return_block(*frame, order);
    observe_pressure();
}

auto PmNode::free_frames(PmFrame::List *list) -> void {
    return_blocks(*list);
    observe_pressure();
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
    // The node's own present and reserved counts went: they were the sum of
    // its zones' and only this function read them. The per-zone lines below
    // carry the same information, once each.
    log::info(
        R"(Node[{}]: 
                start pfn: {}
                spanned frames: {}
            )",
        m_id, m_start_pfn, m_spanned_frames.load());

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

auto PmNode::set_cpu_node(CpuNum const cpu, NodeId const new_nid) -> void {
    DEBUG_ASSERT(cpu < MAX_CPU);
    DEBUG_ASSERT(new_nid < MAX_NODES);

    auto const old_nid = CpuNodeMap::node_of(cpu);
    if (old_nid == new_nid) {
        return;
    }

    if (old_nid != MAX_NODES) {
        if (auto node = PmNode::node(old_nid)) {
            node->m_canary.verify<Self>();
            Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};

            // Erases ourselves from the node to which the CPU belongs.
            node->m_work_cpus.set(cpu, 0);
        }
    }

    // The table before the cache, because the cache asserts it agrees with the
    // table. Neither node's lock is held across both: the two masks are
    // separate invariants, and a CPU appearing in neither for an instant is
    // what a reader of work_cpus() already has to tolerate -- it takes a copy.
    CpuNodeMap::set_node_of(cpu, new_nid);
    if (auto node = PmNode::node(new_nid)) {
        node->m_canary.verify<Self>();
        {
            Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};
            node->m_work_cpus.set(cpu);
        }
        CpuNodeMap::cache_local_node(cpu, node);
    }
}

auto PmNode::publish_cpu(CpuNum const cpu) -> void {
    auto const nid = CpuNodeMap::node_of(cpu);
    if (nid == MAX_NODES) {
        // Not claimed yet. bootstrap() claims only the CPU it runs on;
        // claim_possible_cpus() and the NUMA topology parse claim the rest.
        return;
    }

    auto *const node = PmNode::node(nid);
    DEBUG_ASSERT(node, "CPU {} was claimed by node {}, which does not exist", cpu, nid);
    node->m_canary.verify<Self>();

    {
        Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};
        node->m_work_cpus.set(cpu);
        // PmNode::init() builds the hard affinity from
        // numa::Topology::firmware_node(), but only for the CPUs claimed by
        // then -- the boot CPU. A CPU claimed later joins it here, so that the
        // node's mask says the same thing as the table.
        if (numa::Topology::firmware_node(cpu) == nid) {
            node->m_hard_affinity.set(cpu);
        }
    }
    CpuNodeMap::cache_local_node(cpu, node);
}

/// Publishes each CPU's node into its per-CPU slot, now that the slots exist.
///
/// Not through set_cpu_node(). bootstrap() has already claimed every CPU with
/// CpuNodeMap::seed_from_firmware(), so set_cpu_node() finds the old and
/// new ids equal and returns before it reaches the cache. That is what left
/// the slot null across the zero-page allocation in init_vmm_preheap(): the
/// node-less alloc_frames() went on through a null PmNode, and
/// The allocation walk spun on the ZoneList it read out of address
/// 0x48. The early return is right for what set_cpu_node() is for -- a CPU
/// moving between nodes at run time, where nothing changing means nothing to
/// do -- but the first publish is this function's job, and it has to be
/// unconditional.
///
/// The running CPU is covered by name, the way bootstrap() covers it. The
/// possible mask is filled by the MADT parse, which runs from
/// platform_init_apic() at InitLevel::Irq -- three levels after this hook --
/// so at this point it is empty, and a walk of it alone publishes nothing.
/// That was the other way this used to fail on the same boot.
auto PmNode::publish_cpus() -> void {
    Cpu::possible_mask().for_each(&PmNode::publish_cpu);
    publish_cpu(CpuLocal::current_cpu());
}

/// Every CPU the MADT reported gets a node -- before any of them runs.
///
/// bootstrap() could only claim the CPU it runs on: the possible mask is
/// filled by the MADT parse (platform_init_apic(), InitLevel::Irq), long
/// after the PMM. Every other CPU was left on the MAX_NODES sentinel, and
/// nothing ever moved it -- the only other claimant is the SRAT parse, which
/// a kernel built without NUMA never compiles in and a machine without an
/// SRAT never feeds. So current_node() on an AP answered MAX_NODES, and
/// the first kmalloc() on an AP -- a user thread's channel_create(), on
/// CPU1 -- read m_node_caches[MAX_NODES], one past the end, took the
/// ListMemberHook after the array for a NodeCache, and spun for ever on a list
/// pointer it had mistaken for that NodeCache's lock. The comment on
/// claim_unplaced_cpus() in pm_node_init.cpp describes that exact failure for
/// the boot CPU; this is the same hole for the others.
///
/// A CPU no topology source placed goes where the boot CPU went, which is the
/// first node that owns memory -- the fallback bootstrap() chose, and on a
/// machine without an SRAT the only node there is.
auto PmNode::claim_possible_cpus() -> void {
    auto const fallback = current_node();
    DEBUG_ASSERT(fallback < MAX_NODES, "the boot CPU has no node");

    // The seed is idempotent: a CPU the firmware placed keeps that node, and
    // one it did not lands on |fallback|. Running it again once the MADT has
    // been parsed is how the CPUs that were not in the possible mask during
    // init_pmm() get placed at all.
    // The count is not reported again: bootstrap() already said it for every
    // CPU the firmware bound, and the MADT adds only CPUs the firmware did not.
    static_cast<void>(CpuNodeMap::seed_from_firmware(fallback, node_possible_mask()));
    Cpu::possible_mask().for_each(&PmNode::publish_cpu);
}

/// Once per-CPU storage exists, so that the node-less alloc_frames() can be
/// called at all. CpuLocal + 1 rather than CpuLocal itself: the hooks at a
/// level run when that level is announced, and CpuLocal is announced right
/// after CpuLocal::init() returns -- a hook at exactly that level would run
/// with the units laid out but before main.cpp had said so.
GKTL_INIT_HOOK(PublishCpus, PmNode::publish_cpus, gktl::InitLevel::CpuLocal + 1);

/// After InitLevel::Irq, where the MADT parse fills the possible mask, and
/// before InitLevel::Platform, where x86_bring_aps() starts the CPUs in it.
GKTL_INIT_HOOK(ClaimPossibleCpus, PmNode::claim_possible_cpus, gktl::InitLevel::Irq + 1);

} // namespace ours::mm
