#include <ours/assert.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pm_zone.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_aspace.hpp>

#include <algorithm>
#include <array>

#include <logz4/log.hpp>

namespace ours::mm {
namespace priv {
    // These carry the names pmm.hpp declares and init_pmm.cpp assigns. They
    // were left behind under their old g_-prefixed spelling when the header
    // was renamed, so the definitions the whole tree refers to did not exist.
    extern "C" {
    PhysAddr MAX_PHYS_ADDR;
    PhysAddr MIN_PHYS_ADDR;
    }
} // namespace priv

auto num_nodes() -> usize {
    return PmNode::num_nodes();
}

auto node_work_cpus(NnId nid) -> CpuMask {
    DEBUG_ASSERT(nid < NUMA_MAX_NODE);
    if (auto node = PmNode::node(nid)) {
        return node->work_cpus();
    }
    return CpuMask{};
}

auto node_hard_affinity(NnId nid) -> CpuMask const * {
    DEBUG_ASSERT(nid < NUMA_MAX_NODE);
    if (auto node = PmNode::node(nid)) {
        return &node->hard_affinity();
    }
    return nullptr;
}

auto node_soft_affinity(NnId nid) -> CpuMask const * {
    DEBUG_ASSERT(nid < NUMA_MAX_NODE);
    if (auto node = PmNode::node(nid)) {
        return &node->soft_affinity();
    }
    return nullptr;
}

/// A zeroed page for one level of a page table, from whichever allocator is up.
/// See the declaration in pmm.hpp for why the choice lives here.
auto alloc_page_table(usize size, usize alignment, NnId node) -> PhysAddr {
    if (pmm_enabled()) {
        // Through the node-taking overload, so that `node` means the same thing
        // on both sides of the handover. It used to be read only by the EarlyMem
        // branch below, which made "a table on node N" honoured before the
        // handover and silently ignored after it -- the caller's argument
        // dropped in favour of whatever node the running CPU belongs to.
        //
        // Which node a page table sits on is not cosmetic under NUMA: the CPU
        // that walks it is, by construction, one of the CPUs of the node whose
        // memory it maps, and a table walk is a chain of dependent loads that
        // the hardware cannot overlap.
        //
        // bootmem::ANY_NODE is >= NUMA_MAX_NODE, which this overload forwards to
        // the node-less one, so "no preference" -- every in-tree caller today --
        // behaves exactly as before, the thread's NUMA policy included
        // (PmNode::Current::apply_thread_policy()). The MemoryAllocationState
        // counter applies either way: PmNode::alloc_frames() checks it for
        // every caller.
        auto *frame = alloc_frames(node, GAF_KERNEL | Gaf::Zero, get_order(size / PAGE_SIZE));
        if (!frame) {
            return PhysAddr{0};
        }
        // Mmu, as the tables physboot built are (stamp_reserved_frames()), so
        // that a page-table page says what it is whichever allocator made it;
        // with its body cleared, so that it starts with no tables set aside
        // (MmuBody).
        frame->become<page::MmuBody>();
        return frame_to_phys(frame);
    }

    // Zeroed by hand: the boot allocator has no notion of Gaf, and a page table
    // whose unused entries are leftover bytes is a set of mappings nobody made.
    auto *table = EarlyMem::allocate<u8>(size, alignment, node, bootmem::MemoryType::PageTable);
    if (!table) {
        return PhysAddr{0};
    }
    std::fill_n(table, size, u8(0));

    // EarlyMem hands back a physmap address, which is what virt_to_phys inverts.
    return PhysMap::virt_to_phys(VirtAddr(table));
}

auto free_page_table(PhysAddr base, usize size) -> void {
    // Nothing to do before the buddy: see the header. Freeing an EarlyMem
    // allocation would hand the boot allocator a range it never considered
    // free, and after do_handoff() there is no boot allocator left to tell.
    if (!pmm_enabled()) {
        return;
    }
    // Handed back still Mmu, as every caller of free_frames() hands frames
    // back in the role they were used in; only the buddy's insert may say
    // Pmm. Anything else here is a frame that was never a page table, which
    // the assertion below catches.
    auto *const frame = phys_to_frame(base);
    DEBUG_ASSERT(frame->is_role(page::Role::Mmu),
                 "freeing {:#x} as a page table, but its role is {}", base,
                 to_string(frame->role()));
    // A table that still has tables set aside for its large leaves would take
    // them with it, never to be freed: its owner gives them back first
    // (page_tables::X86PageTableImpl::free_tables(), through
    // discard_all_tables()).
    DEBUG_ASSERT(frame->as<page::MmuBody>().deposits == nullptr,
                 "freeing the page table {:#x} with tables still set aside for it", base);
    free_frames(frame, get_order(size / PAGE_SIZE));
}

auto free_frame_count() -> usize {
    usize total = 0;
    // The node's own zones, not its fallback order: that one lists other
    // nodes' zones too, and would count them once per node.
    PmNode::for_each_nid(PmNode::State::Online, [&total](NnId const nid) {
        auto *const node = PmNode::node(nid);
        if (!node) {
            return;
        }
        node->zone_list()->for_each_local_zone(
            [&total](PmZone &zone) { total += zone.count_free_frames(); });
    });
    return total;
}

/// Called from kmalloc and MemCache::allocate to resolve "no preference" into a
/// real node.
auto current_node() -> NnId {
    return PmNode::Topology::current_node();
}

auto node_online_mask() -> NodeMask const & {
    return PmNode::node_online_mask();
}

auto node_possible_mask() -> NodeMask const & {
    return PmNode::node_possible_mask();
}

auto alloc_frames(Gaf gaf, usize order, NodeMask const &fallback) -> PmFrame * {
    auto result = PmNode::Current::alloc_frames(gaf, order, fallback);
    if (!result) {
        log::trace("Failed to allocate frame with reason: {}", to_string(result.unwrap_err()));
        return nullptr;
    }
    return *result;
}

auto alloc_frames(Gaf gaf, PhysAddr *addr, usize order) -> PmFrame * {
    auto *const frame = alloc_frames(gaf, order, node_online_mask());
    if (frame && addr) {
        *addr = frame_to_phys(frame);
    }
    return frame;
}

auto alloc_frames_fragment(Gaf gaf, usize n, NodeMask const &fallback)
    -> ktl::Result<PmFrame::List> {
    auto result = PmNode::Current::alloc_frames_fragment(gaf, n, fallback);
    if (!result) {
        log::trace("Failed to allocate frame with reason: {}", to_string(result.unwrap_err()));
    }
    return ustl::move(result);
}

auto alloc_frames(NnId nid, Gaf gaf, PmFrame::List *list, usize n, NodeMask const &nodes)
    -> Status {
    // NUMA_MAX_NODE means "no preference": use the node the current CPU
    // belongs to, which is what the node-less overloads do.
    if (nid >= NUMA_MAX_NODE) {
        auto result = PmNode::Current::alloc_frames_bulk(gaf, n, nodes);
        if (!result) {
            return result.unwrap_err();
        }
        *list = ustl::move(*result);
        return Status::Ok;
    }

    auto const node = PmNode::node(nid);
    if (!node) {
        return Status::InvArgs;
    }

    auto result = node->alloc_frames_bulk(gaf, n, nodes);
    if (!result) {
        log::trace("Failed to allocate {} frames with reason: {}", n,
                   to_string(result.unwrap_err()));
        return result.unwrap_err();
    }

    *list = ustl::move(*result);
    return Status::Ok;
}

auto alloc_frames(NnId nid, Gaf gaf, usize order, NodeMask const &nodes) -> PmFrame * {
    if (nid >= NUMA_MAX_NODE) {
        return alloc_frames(gaf, order, nodes);
    }

    auto const node = PmNode::node(nid);
    if (!node) {
        return nullptr;
    }

    auto result = node->alloc_frames(gaf, order, nodes);
    if (!result) {
        log::trace("Failed to allocate frame on node {} with reason: {}", nid,
                   to_string(result.unwrap_err()));
        return nullptr;
    }
    return *result;
}

auto free_frames(PmFrame *frame, usize order) -> void {
    log::trace("Free frame({:X}, {})", frame_to_phys(frame), order);
    PmNode::free_frames(frame, order);
}

auto free_frames(PmFrame::List *list) -> void {
    PmNode::free_frames(list);
}

auto unwire_frame(PmFrame *const frame) -> void {
    DEBUG_ASSERT(!!(frame->flags() & page::Flags::Reserved), "frame {:#x} is not reserved",
                 frame_to_phys(frame));
    DEBUG_ASSERT(frame->is_role(page::Role::None), "reserved frame {:#x} has a role",
                 frame_to_phys(frame));
    DEBUG_ASSERT(!frame->is_linked(), "reserved frame {:#x} is on a list", frame_to_phys(frame));

    frame->clear_reserved();
    if (auto *const node = PmNode::node(frame->nid())) {
        node->sub_reserved_frames(1);
        if (auto *const zone = node->zone_list()->local_zone(frame->zone())) {
            zone->sub_reserved_frames(1);
        }
    }
}

} // namespace ours::mm