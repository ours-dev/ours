#include <ours/mm/cpu-local.hpp>

#include <ours/mm/init.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/pm-node.hpp>
#include <ours/mm/pmm.hpp>

#include <gktl/init_hook.hpp>
#include <gktl/defer.hpp>
#include <gktl/batch.hpp>

#include <ustl/mem/box.hpp>
#include <ustl/views/span.hpp>
#include <ustl/util/type-map.hpp>
#include <ustl/mem/align.hpp>

using ustl::mm::align_up;

extern char const kInitialCpuLocalStart[] LINK_NAME("__cpu_local_start");
extern char const kInitialCpuLocalEnd[] LINK_NAME("__cpu_local_end");

namespace ours::mm {
namespace {
    FORCE_INLINE
    auto initial_area_size() -> usize {
        return kInitialCpuLocalEnd - kInitialCpuLocalStart;
    }

    FORCE_INLINE
    auto is_in_initial_area(VirtAddr va) -> bool {
        return va >= VirtAddr(kInitialCpuLocalStart) && va < VirtAddr(kInitialCpuLocalEnd);
    }
} // namespace anonymous

    struct DynChunk: ustl::collections::intrusive::ListBaseHook<> {
        VirtAddr base;
        VirtAddr size;
        VirtAddr base_aligned;
        VirtAddr size_aligned;

        u32 num_pages;
        u32 num_pages_populated;
    };

    class CpuLocal::Mapper {
        using Self = CpuLocal::Mapper;
      public:
        enum class AddrType: u8 {
            Static,
            Dynamic,
        };

        /// Translate a cpu local address to available virtual address.
        ///
        /// Return 0 if failed.
        auto translate(VirtAddr va) -> VirtAddr;

        /// 
        auto record(usize) -> void;

        auto search_chunk(VirtAddr va) -> DynChunk *;

        auto init(u32 num_groups, u32 num_units, usize unit_align, usize dyn_size) -> bool;
      private:
        auto get_addr_type(VirtAddr va) -> AddrType {
            if (va >= m_base_addr && va - m_base_addr < initial_area_size()) {
                return AddrType::Static;
            } 
            return AddrType::Dynamic;
        }

        auto num_groups() const -> u32 { return m_offsets_per_group.size(); }

        auto num_units() const -> u32 { return m_offsets_per_unit.size(); }

        auto create_first_chunk() -> Status;

        auto compactify_sparse_topology_ids() -> void;

        /// Invariants:
        ///     1). First chunk is
        ustl::collections::intrusive::List<DynChunk> m_dynchunks;
        DynChunk m_first_chunk;

        VirtAddr m_base_addr;
        usize m_unit_size;

        ustl::views::Span<u32> m_cpu_to_unit;  // CPU -> Unit
        ustl::views::Span<u32> m_unit_to_cpu;  // Unit -> CPU 
        ustl::views::Span<u32> m_node_to_group;  // Node -> Group
        ustl::views::Span<u32> m_group_to_node;  // Group -> Node

        ustl::views::Span<isize> m_offsets_per_unit;
        ustl::views::Span<isize> m_offsets_per_group;
        ustl::views::Span<u32> m_num_units_per_group;
        ustl::views::Span<u32> m_group_consumed; // UNIT(Kib)
        ustl::views::Span<PmFrame::List> m_group_frames;
    };

    auto CpuLocal::Mapper::init(u32 num_groups, u32 num_units, usize unit_align, usize dyn_size) -> bool {
        using ustl::traits::RemoveCvRefT;
        using gktl::action::MixedParamsPacket;
        using gktl::action::CompileParamsPacket;
        using gktl::action::Param;

        ustl::Array<ustl::mm::Box<char[]>, 8> boxes;

        auto const success = gktl::action::batch<
            CompileParamsPacket<&Self::m_cpu_to_unit, MAX_CPU>,
            CompileParamsPacket<&Self::m_node_to_group, NUMA_MAX_NODE>
        >(
            [&] <int Index> (auto member_ptr, usize num_elements) { // Predicate
                auto raw = new (mm::kGafKernel) RemoveCvRefT<decltype(m_unit_to_cpu[0])>[num_elements];
                boxes[Index].reset(reinterpret_cast<char *>(raw));
                return boxes[Index] != nullptr;
            },
            [&] <int Index> (auto member_ptr, usize num_elements) { // On Compelete
                auto raw = reinterpret_cast<RemoveCvRefT<decltype(m_unit_to_cpu[0])> *>(boxes[Index].release());
                this->*member_ptr = ustl::views::make_span(raw, num_elements);
            },
            [&] () {}, // OnError
            MixedParamsPacket<Param<&Self::m_unit_to_cpu>, u32>(num_units),
            MixedParamsPacket<Param<&Self::m_group_to_node>, u32>(num_groups),
            MixedParamsPacket<Param<&Self::m_offsets_per_unit>, u32>(num_units),
            MixedParamsPacket<Param<&Self::m_offsets_per_group>, u32>(num_groups),
            MixedParamsPacket<Param<&Self::m_num_units_per_group>, u32>(num_groups),
            MixedParamsPacket<Param<&Self::m_group_consumed>, u32>(num_groups)
        );

        if (success) {
            compactify_sparse_topology_ids();
        }
        return success;
    }

    /// Time Compelexity: O(m * n)
    /// Space Compelexity: O(m + n)
    auto CpuLocal::Mapper::compactify_sparse_topology_ids() -> void {
        auto &nodes = mm::node_possible_mask();
        for (auto nid = 0, uid = 0, gid = 0; nid < nodes.size(); ++nid) {
            if (!nodes.test(nid)) {
                continue;
            }

            auto cpus = mm::node_hard_affinity(nid);
            for (auto cpu = 0; cpu < cpus->size(); ++cpu) {
                if (!cpus->test(cpu)) {
                    continue;
                }

                m_cpu_to_unit[cpu] = uid;
                m_unit_to_cpu[uid] = cpu;
            }

            m_node_to_group[nid] = gid;
            m_group_to_node[gid] = nid;
        }
    }

    auto CpuLocal::Mapper::create_first_chunk() -> Status {
        auto const nr_groups = num_groups();

        mm::PmFrame::List frames;
        m_base_addr = ustl::NumericLimits<VirtAddr>::max();
        for (auto group = 0; group < nr_groups; ++group) {
            auto const nid = m_group_to_node[group];
            auto const nr_units = m_num_units_per_group[group];

            auto const num_frames = align_up(m_unit_size * nr_units, PAGE_SIZE) / PAGE_SIZE;
            auto const order = mm::get_order(num_frames);
            auto const padding = (PAGE_SIZE * (1 << order) - m_unit_size) / nr_units;

            auto frame = alloc_frames(nid, mm::kGafBoot, order);
            if (!frame) {
                free_frames(&frames);
                return Status::OutOfMem;
            }

            m_group_frames[group].push_back(*frame);
            m_group_consumed[group] = BIT(order);

            m_base_addr = ustl::algorithms::min(m_base_addr, frame_to_virt(frame));
        }

        for (auto group = 0, unit = 0; group < nr_groups; ++group) {
            auto const addr = frame_to_virt(&m_group_frames[group].front());
            m_offsets_per_group[group] = addr - m_base_addr;

            for (auto i = 0; i < m_num_units_per_group[group]; ++i, ++unit) {
                m_offsets_per_unit[unit] = addr + m_unit_size * i - m_base_addr;
            }
        }

        return Status::Ok;
    }

    /// Runtime cpu local allocator.
    class CpuLocal::Dyn {
        using Self = CpuLocal::Dyn;
      public:
        auto allocate(usize size, usize align, bool reserved, Gaf gaf) -> void *;
        auto deallocate(void *) -> void;
      private:
        Mapper m_mapper;
    };
    
    decltype(CpuLocal::g_states) CpuLocal::g_states;
    decltype(CpuLocal::g_cpu_offsets) CpuLocal::g_cpu_offsets;

    CPU_LOCAL
    decltype(CpuLocal::g_current_cpunum) CpuLocal::g_current_cpunum = kInvalidCpuNum;

    CPU_LOCAL
    decltype(CpuLocal::g_current_cpu_offset) CpuLocal::g_current_cpu_offset = 0;

    auto CpuLocal::bootstrap(CpuMask const &cpus) -> void {
        DEBUG_ASSERT(get_init_stage() == InitStage::Pmm);
    }

} // namespace ours::mm