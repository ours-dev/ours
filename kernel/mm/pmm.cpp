#include <ours/assert.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_node.hpp>
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

/// Declared in pmm.hpp and called from kmalloc and MemCache::allocate to
/// resolve "no preference" into a real node, but never defined until now.
auto alloc_page_table(usize size, usize alignment, NnId node) -> PhysAddr {
    if (pmm_enabled()) {
        PhysAddr phys;
        auto *frame = alloc_frames(GAF_KERNEL | Gaf::Zero, &phys, get_order(size / PAGE_SIZE));
        return frame ? phys : PhysAddr{0};
    }

    // Zeroed by hand: the boot allocator has no notion of Gaf, and a page table
    // whose unused entries are leftover bytes is a set of mappings nobody made.
    auto *table = EarlyMem::allocate<u8>(size, alignment, node);
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
    free_frames(phys_to_frame(base), get_order(size / PAGE_SIZE));
}

auto current_node() -> NnId {
    return PmNode::Current::nid();
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

} // namespace ours::mm