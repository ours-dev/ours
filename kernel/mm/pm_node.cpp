#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/node_placement.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/priv/zone_selector.hpp>
#include <ours/panic.hpp>
#include <ours/task/thread.hpp>

#include <algorithm>

#include <gktl/counter.hpp>
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


auto PmNode::get_frame_from_zone_list(AllocRequest const &request) -> PmFrame * {
    // The fallback order, not this node's zones by type: Gaf::ThisNode is
    // honoured by the request's node mask, which resolve_request() has already
    // narrowed, so the candidate list is the same either way and the filter
    // does the restricting. The walk that was deleted with ZoneIterator chose
    // between two lists instead, which is a second mechanism for one intent.
    ZoneSelector const selector{m_zone_list.fallback(), request};
    for (auto &zone: selector) {
        // The zone answers with an Option, which is unwrapped here because
        // PmNode's own return types are a pointer for now; this is where the
        // two conventions meet, and it is one line rather than a convention
        // of its own.
        if (auto const frame = zone.alloc_frames(request)) {
            return &frame.value();
        }
    }
    return nullptr;
}

auto PmNode::alloc_frames_postamble(PmFrame *frame, AllocRequest const &request) -> void {
    if (current_node() == m_id) {
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
    if (!task::Thread::Current::memory_allocation_state().is_enabled()) {
        return ustl::err(Status::AccessDenied);
    }

    if (order > MAX_FRAME_ORDER) {
        return ustl::err(Status::InvArgs);
    }

    // One value, built once, read by every layer below. It replaces a
    // MemoryAllocationContext filled by a preamble that could only answer Ok,
    // and an AllocationOptions derived by a mapping that answered with the
    // empty set for every Gaf.
    auto const request =
        resolve_request(gaf, order, fallback, Origin{.node = m_id, .cpu = Cpu::id()});

    // The only attempt. Nothing reclaims on this path, by design: taking
    // frames back means reaching into whatever holds them -- VMO pages, the
    // page queues, the object cache's slabs -- and every one of those sits
    // above this allocator, so a reclaimer called from inside it would invert
    // that order. Reclaim belongs above the page queues instead, watching this
    // node's free-frame level and handing frames back down, rather than being
    // a step of an allocation that has already failed.
    //
    // What stood here was a loop over two stubs that both returned null, so a
    // caller carrying Gaf::DirectReclaim -- which is every GAF_KERNEL
    // allocation -- spun in it rather than being refused. The loop could not
    // have helped even with the stubs filled in from here: its retry asked the
    // same zones the same question with the same arguments, because nothing
    // between the caller's Gaf and the zones differed on a second pass.
    //
    // A relaxed retry is a real thing and this is where it goes when it
    // exists: an attempt that lets a caller dip below a zone's high watermark
    // will carry a different mark on the request, and so have a reason to be a
    // step rather than a repeat.
    auto *const frame = get_frame_from_zone_list(request);
    if (!frame) {
        // The zone walk answers with a plain pointer, null on failure, so this
        // is where that becomes the error the public interface promises.
        // Wrapping the null in ustl::ok() made every caller's `if (result)`
        // true -- the operator is is_ok(), not a test of the pointer -- and the
        // bulk and fragment loops below then dereferenced it to reach
        // push_back().
        //
        // Ahead of the postamble, which counts whether the frame came from the
        // node the caller is running on. A failure is neither a local hit nor a
        // miss, and handing it to either counter would turn both of them into
        // counts of attempts rather than of allocations.
        return ustl::err(Status::OutOfMem);
    }

    alloc_frames_postamble(frame, request);
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
    // The runtime half of the guard in early_mem.cpp's free_frames_phys_range():
    // a reserved frame reaches the buddy only through unwire_frame(). Nothing
    // else on this path looks at the frame, so without this a reserved frame
    // would be handed out again still flagged and still counted.
    DEBUG_ASSERT(!(frame->flags() & page::Flags::Reserved),
                 "frame {:#x} is reserved; unwire_frame() it before freeing", frame_to_phys(frame));
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

    auto const old_nid = NodePlacement::node_of(cpu);
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
    NodePlacement::set_node_of(cpu, new_nid);
    if (auto node = PmNode::node(new_nid)) {
        node->m_canary.verify<Self>();
        {
            Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};
            node->m_work_cpus.set(cpu);
        }
        NodePlacement::cache_local_node(cpu, node);
    }
}

auto PmNode::publish_cpu(CpuNum const cpu) -> void {
    auto const nid = NodePlacement::node_of(cpu);
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
    NodePlacement::cache_local_node(cpu, node);
}

/// Publishes each CPU's node into its per-CPU slot, now that the slots exist.
///
/// Not through set_cpu_node(). bootstrap() has already claimed every CPU with
/// NodePlacement::seed_from_firmware(), so set_cpu_node() finds the old and
/// new ids equal and returns before it reaches the cache. That is what left
/// the slot null across the zero-page allocation in init_vmm_preheap(): the
/// node-less alloc_frames() went on through a null PmNode, and
/// get_frame_from_zone_list() spun on the ZoneList it read out of address
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
    NodePlacement::seed_from_firmware(fallback);
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
