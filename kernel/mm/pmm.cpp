#include <ours/mm/pm-node.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/memory-model.hpp>
#include <ours/assert.hpp>
#include <ours/mm/cpu-local.hpp>

#include <ustl/array.hpp>
#include <logz4/log.hpp>

namespace ours::mm {
namespace priv {
    NO_MANGLE {
        PhysAddr g_max_phys_addr;
        PhysAddr g_min_phys_addr;
    }
} // namespace priv

    auto num_nodes() -> usize {
        return PmNode::num_nodes();
    }

    auto node_work_cpus(NnId nid) -> CpuMask const * {
        DEBUG_ASSERT(nid < NUMA_MAX_NODE);
        if (auto node = PmNode::node(nid)) {
            return &node->work_cpus();
        }
        return nullptr; 
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
            return 0;
        }
        return *result;
    }

    auto alloc_frames_fragment(Gaf gaf, usize n, NodeMask const &fallback) -> ktl::Result<PmFrame::List> {
        auto result = PmNode::Current::alloc_frames_fragment(gaf, n, fallback);
        if (!result) {
            log::trace("Failed to allocate frame with reason: {}", to_string(result.unwrap_err()));
        }
        return ustl::move(result);
    }

    auto alloc_frames(NnId nid, Gaf gaf, PmFrame::List *list, usize n, NodeMask const &nodes) -> Status {
        if (nid >= NUMA_MAX_NODE) {
            return alloc_frames(gaf, list, n, nodes);
        }
    }

    auto alloc_frame(NnId nid, Gaf gaf, usize order, NodeMask const &nodes) -> PmFrame * {
        if (nid >= NUMA_MAX_NODE) {
            return alloc_frame(gaf, order, nodes);
        }
    }

    auto free_frame(PmFrame *frame, usize order) -> void {
        log::trace("Free frame({:X}, {})", frame_to_phys(frame), order);
        PmNode::Current::free_frames(frame, order);
    }

    auto free_frames(PmFrame::List *list) -> void {
        PmNode::free_frames(list);
    }

} // namespace ours::mm