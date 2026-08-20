#include <ours/mm/cpu-local.hpp>

#include <ours/mm/cfg.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/pm-node.hpp>
#include <ours/mm/pmm.hpp>

#include <ktl/kmalloc.hpp>
#include <ktl/new.hpp>

#include <ustl/algorithms/copy.hpp>
#include <ustl/algorithms/minmax.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/views/span.hpp>

using ustl::mm::align_up;

extern char const kInitialCpuLocalStart[] LINK_NAME("__cpu_local_start");
extern char const kInitialCpuLocalEnd[] LINK_NAME("__cpu_local_end");

namespace ours::mm {
namespace {
    /// Allocation granularity of the dynamic area. Every reservation is a whole
    /// number of cells so the bitmap has one bit per cell.
    constexpr
    auto const kDynCellSize = usize(sizeof(usize));

    constexpr
    auto const kInvalidUnit = u32(-1);

    FORCE_INLINE
    auto initial_area_size() -> usize {
        return kInitialCpuLocalEnd - kInitialCpuLocalStart;
    }

    FORCE_INLINE
    auto canonical_base() -> VirtAddr {
        return VirtAddr(kInitialCpuLocalStart);
    }

    FORCE_INLINE
    auto is_in_initial_area(VirtAddr va) -> bool {
        return va >= VirtAddr(kInitialCpuLocalStart) && va < VirtAddr(kInitialCpuLocalEnd);
    }

    /// Allocates `n` default-constructed `T` from the kernel heap.
    ///
    /// Returns an empty span on failure.
    template <typename T>
    auto make_span(usize n) -> ustl::views::Span<T> {
        auto raw = static_cast<T *>(ktl::kmalloc(n * sizeof(T), kGafKernel));
        if (!raw) {
            return {};
        }

        for (auto i = usize(0); i < n; ++i) {
            new (static_cast<void *>(raw + i)) T();
        }
        return ustl::views::make_span(raw, n);
    }

    template <typename T>
    auto drop_span(ustl::views::Span<T> &span) -> void {
        if (span.empty()) {
            return;
        }

        for (auto &item : span) {
            item.~T();
        }
        ktl::kfree(span.data());
        span = {};
    }
} // namespace anonymous

    struct DynChunk: ustl::collections::intrusive::ListBaseHook<> {
        /// Canonical address of the first byte this chunk manages.
        VirtAddr base;
        usize size;

        /// One bit per `kDynCellSize` bytes; set means taken.
        ustl::views::Span<usize> bitmap;

        usize num_cells;
        usize num_cells_used;
    };

    /// Owns the per-CPU units and the topology maps that place them.
    ///
    /// A `group` is a NUMA node that owns at least one possible CPU; a `unit` is
    /// one CPU's slice of its group's allocation. Both are numbered densely so
    /// sparse APIC/node ids do not waste memory.
    class CpuLocal::Mapper {
        using Self = CpuLocal::Mapper;

      public:
        enum class AddrType: u8 {
            Static,
            Dynamic,
        };

        auto init() -> Status;

        /// Canonical offset of `unit`, i.e. `unit_base - __cpu_local_start`.
        auto offset_of_unit(u32 unit) const -> isize {
            return m_offsets_per_unit[unit];
        }

        auto unit_of_cpu(CpuNum cpu) const -> u32 {
            return m_cpu_to_unit[cpu];
        }

        auto unit_size() const -> usize {
            return m_unit_size;
        }

        /// Canonical bounds of the dynamic area inside every unit.
        auto dyn_base() const -> VirtAddr {
            return canonical_base() + align_up(initial_area_size(), kDynCellSize);
        }

        auto dyn_size() const -> usize {
            return m_unit_size - (dyn_base() - canonical_base());
        }

        auto get_addr_type(VirtAddr va) const -> AddrType {
            return is_in_initial_area(va) ? AddrType::Static : AddrType::Dynamic;
        }

        auto num_groups() const -> u32 {
            return u32(m_offsets_per_group.size());
        }

        auto num_units() const -> u32 {
            return u32(m_offsets_per_unit.size());
        }

      private:
        auto create_first_chunk() -> Status;

        auto compactify_sparse_topology_ids() -> void;

        auto release() -> void;

        usize m_unit_size;

        ustl::views::Span<u32> m_cpu_to_unit;    // CPU -> Unit
        ustl::views::Span<u32> m_unit_to_cpu;    // Unit -> CPU
        ustl::views::Span<u32> m_node_to_group;  // Node -> Group
        ustl::views::Span<u32> m_group_to_node;  // Group -> Node

        ustl::views::Span<isize> m_offsets_per_unit;
        ustl::views::Span<isize> m_offsets_per_group;
        ustl::views::Span<u32> m_num_units_per_group;
        ustl::views::Span<PmFrame::List> m_group_frames;
    };

    auto CpuLocal::Mapper::init() -> Status {
        auto const nr_units = u32(Cpu::possible_mask().count());
        if (!nr_units) {
            return Status::InvalidArguments;
        }

        // A node earns a group only if it owns a possible CPU, so the upper
        // bound is the smaller of the two counts.
        auto nr_groups = u32(0);
        auto const &nodes = mm::node_possible_mask();
        for (auto nid = usize(0); nid < nodes.size(); ++nid) {
            if (nodes.test(nid)) {
                ++nr_groups;
            }
        }
        if (!nr_groups) {
            nr_groups = 1;
        }
        // One spare slot so CPUs that no node claims can still form a group.
        ++nr_groups;

        m_unit_size = align_up(initial_area_size() + ArchCpuLocal::kDynFirstChunkSize,
                               ArchCpuLocal::kUnitAlign);

        m_cpu_to_unit = make_span<u32>(MAX_CPU);
        m_unit_to_cpu = make_span<u32>(nr_units);
        m_node_to_group = make_span<u32>(NUMA_MAX_NODE);
        m_group_to_node = make_span<u32>(nr_groups);
        m_offsets_per_unit = make_span<isize>(nr_units);
        m_offsets_per_group = make_span<isize>(nr_groups);
        m_num_units_per_group = make_span<u32>(nr_groups);
        m_group_frames = make_span<PmFrame::List>(nr_groups);

        if (m_cpu_to_unit.empty() || m_unit_to_cpu.empty() || m_node_to_group.empty() ||
            m_group_to_node.empty() || m_offsets_per_unit.empty() ||
            m_offsets_per_group.empty() || m_num_units_per_group.empty() ||
            m_group_frames.empty()) {
            release();
            return Status::OutOfMem;
        }

        for (auto &unit : m_cpu_to_unit) {
            unit = kInvalidUnit;
        }

        compactify_sparse_topology_ids();

        auto const status = create_first_chunk();
        if (Status::Ok != status) {
            release();
        }
        return status;
    }

    auto CpuLocal::Mapper::release() -> void {
        drop_span(m_cpu_to_unit);
        drop_span(m_unit_to_cpu);
        drop_span(m_node_to_group);
        drop_span(m_group_to_node);
        drop_span(m_offsets_per_unit);
        drop_span(m_offsets_per_group);
        drop_span(m_num_units_per_group);
        drop_span(m_group_frames);
    }

    /// Numbers nodes and CPUs densely, keeping every group's units contiguous so
    /// one allocation per group can back all of them.
    ///
    /// Time Complexity: O(m * n)
    /// Space Complexity: O(m + n)
    auto CpuLocal::Mapper::compactify_sparse_topology_ids() -> void {
        auto const &cpus = Cpu::possible_mask();
        auto const &nodes = mm::node_possible_mask();

        auto uid = u32(0);
        auto gid = u32(0);
        for (auto nid = usize(0); nid < nodes.size() && gid < num_groups(); ++nid) {
            if (!nodes.test(nid)) {
                continue;
            }

            auto units_here = u32(0);
            for (auto cpu = CpuNum(0); cpu < cpus.size(); ++cpu) {
                if (!cpus.test(cpu) || mm::cpu_to_node(cpu) != NnId(nid)) {
                    continue;
                }

                m_cpu_to_unit[cpu] = uid;
                m_unit_to_cpu[uid] = cpu;
                ++uid;
                ++units_here;
            }

            // A node with no CPUs of its own never becomes a group.
            if (!units_here) {
                continue;
            }

            m_node_to_group[nid] = gid;
            m_group_to_node[gid] = u32(nid);
            m_num_units_per_group[gid] = units_here;
            ++gid;
        }

        // Any possible CPU that no node claimed still needs a unit, so a broken
        // or absent SRAT cannot leave one unmapped. They go into a group of
        // their own: a group's units have to stay contiguous, and `uid` has
        // already moved past every group placed above.
        auto orphans = u32(0);
        for (auto cpu = CpuNum(0); cpu < cpus.size(); ++cpu) {
            if (!cpus.test(cpu) || m_cpu_to_unit[cpu] != kInvalidUnit) {
                continue;
            }

            m_cpu_to_unit[cpu] = uid;
            m_unit_to_cpu[uid] = cpu;
            ++uid;
            ++orphans;
        }

        if (orphans) {
            DEBUG_ASSERT(gid < num_groups(), "No group slot left for unclaimed CPUs");
            m_group_to_node[gid] = 0;
            m_num_units_per_group[gid] = orphans;
        }
    }

    auto CpuLocal::Mapper::create_first_chunk() -> Status {
        auto const nr_groups = num_groups();

        for (auto group = u32(0); group < nr_groups; ++group) {
            auto const nr_units = m_num_units_per_group[group];
            if (!nr_units) {
                continue;
            }

            auto const nid = NnId(m_group_to_node[group]);
            auto const num_frames = align_up(m_unit_size * nr_units, PAGE_SIZE) / PAGE_SIZE;
            auto const order = mm::get_order(num_frames);

            auto frame = alloc_frames(nid, mm::kGafBoot, order);
            if (!frame) {
                for (auto done = u32(0); done < group; ++done) {
                    free_frames(&m_group_frames[done]);
                }
                return Status::OutOfMem;
            }

            m_group_frames[group].push_back(*frame);
        }

        for (auto group = u32(0), unit = u32(0); group < nr_groups; ++group) {
            auto const nr_units = m_num_units_per_group[group];
            if (!nr_units) {
                continue;
            }

            auto const addr = frame_to_virt(&m_group_frames[group].front());
            m_offsets_per_group[group] = isize(addr - canonical_base());

            for (auto i = u32(0); i < nr_units; ++i, ++unit) {
                m_offsets_per_unit[unit] = isize(addr + m_unit_size * i - canonical_base());
            }
        }

        return Status::Ok;
    }

    /// Runtime cpu local allocator.
    ///
    /// Hands out canonical addresses from the dynamic tail of every unit. One
    /// reservation covers all CPUs at once, so a single bitmap describes them.
    class CpuLocal::Dyn {
        using Self = CpuLocal::Dyn;

      public:
        auto init(Mapper &mapper) -> Status;

        auto allocate(usize size, usize align) -> VirtAddr;

        auto deallocate(VirtAddr canonical, usize size) -> void;

        auto owns(VirtAddr canonical) const -> bool;

      private:
        static auto test(DynChunk const &chunk, usize cell) -> bool {
            return (chunk.bitmap[cell / kWordBits] & (usize(1) << (cell % kWordBits))) != 0;
        }

        static auto set(DynChunk &chunk, usize cell, bool value) -> void {
            auto const mask = usize(1) << (cell % kWordBits);
            if (value) {
                chunk.bitmap[cell / kWordBits] |= mask;
            } else {
                chunk.bitmap[cell / kWordBits] &= ~mask;
            }
        }

        constexpr
        static auto const kWordBits = usize(sizeof(usize) * 8);

        ustl::collections::intrusive::List<DynChunk> m_dynchunks;
        DynChunk m_first_chunk;
    };

    auto CpuLocal::Dyn::init(Mapper &mapper) -> Status {
        auto const size = mapper.dyn_size();
        auto const num_cells = size / kDynCellSize;
        auto const num_words = (num_cells + kWordBits - 1) / kWordBits;

        m_first_chunk.bitmap = make_span<usize>(num_words);
        if (m_first_chunk.bitmap.empty()) {
            return Status::OutOfMem;
        }

        m_first_chunk.base = mapper.dyn_base();
        m_first_chunk.size = size;
        m_first_chunk.num_cells = num_cells;
        m_first_chunk.num_cells_used = 0;

        m_dynchunks.push_back(m_first_chunk);
        return Status::Ok;
    }

    auto CpuLocal::Dyn::owns(VirtAddr canonical) const -> bool {
        for (auto const &chunk : m_dynchunks) {
            if (canonical >= chunk.base && canonical < chunk.base + chunk.size) {
                return true;
            }
        }
        return false;
    }

    auto CpuLocal::Dyn::allocate(usize size, usize align) -> VirtAddr {
        if (!size) {
            return 0;
        }

        auto const cells = align_up(size, kDynCellSize) / kDynCellSize;
        auto const align_bytes = ustl::algorithms::max(align, kDynCellSize);
        DEBUG_ASSERT(!(align_bytes & (align_bytes - 1)), "Alignment must be a power of two");

        auto const step = align_bytes / kDynCellSize;

        for (auto &chunk : m_dynchunks) {
            if (chunk.num_cells_used + cells > chunk.num_cells) {
                continue;
            }

            // Alignment is a property of the address, not of the cell index: the
            // chunk base is only cell-aligned, so anchor the scan on the first
            // cell whose address satisfies `align_bytes` and step in whole
            // multiples of it from there.
            auto const first = (align_up(chunk.base, align_bytes) - chunk.base) / kDynCellSize;

            for (auto start = first; start + cells <= chunk.num_cells; start += step) {
                auto taken = false;
                for (auto i = usize(0); i < cells; ++i) {
                    if (test(chunk, start + i)) {
                        taken = true;
                        // Resume past the blocking cell, keeping the alignment
                        // residue established by `first`.
                        start = first + align_up(start + i + 1 - first, step) - step;
                        break;
                    }
                }
                if (taken) {
                    continue;
                }

                for (auto i = usize(0); i < cells; ++i) {
                    set(chunk, start + i, true);
                }
                chunk.num_cells_used += cells;
                return chunk.base + start * kDynCellSize;
            }
        }

        return 0;
    }

    auto CpuLocal::Dyn::deallocate(VirtAddr canonical, usize size) -> void {
        if (!canonical || !size) {
            return;
        }

        auto const cells = align_up(size, kDynCellSize) / kDynCellSize;
        for (auto &chunk : m_dynchunks) {
            if (canonical < chunk.base || canonical >= chunk.base + chunk.size) {
                continue;
            }

            auto const start = (canonical - chunk.base) / kDynCellSize;
            DEBUG_ASSERT(start + cells <= chunk.num_cells, "Freeing past the end of a chunk");
            for (auto i = usize(0); i < cells; ++i) {
                DEBUG_ASSERT(test(chunk, start + i), "Double free of cpu local storage");
                set(chunk, start + i, false);
            }
            chunk.num_cells_used -= cells;
            return;
        }

        DEBUG_ASSERT(false, "Freeing an address that is not cpu local storage");
    }

    decltype(CpuLocal::g_states) CpuLocal::g_states;
    decltype(CpuLocal::g_cpu_offsets) CpuLocal::g_cpu_offsets;

    CPU_LOCAL
    decltype(CpuLocal::g_current_cpunum) CpuLocal::g_current_cpunum = INVALID_CPU_NUM;

    CPU_LOCAL
    decltype(CpuLocal::g_current_cpu_offset) CpuLocal::g_current_cpu_offset = 0;

    CpuLocal::Mapper *CpuLocal::g_mapper;
    CpuLocal::Dyn *CpuLocal::g_dyn;

    /// Order matters: the base register has to be live before any `read`/`write`
    /// can reach the unit, so it is installed first and the bookkeeping written
    /// through it afterwards.
    auto CpuLocal::attach_to_unit(CpuNum cpunum, isize offset) -> void {
        ArchCpuLocal::install(offset);
        Self::write(g_current_cpu_offset, offset);
        Self::write(g_current_cpunum, cpunum);
    }

    auto CpuLocal::init_early() -> void {
        auto const cpu = Cpu::id();

        // Before the allocator exists there is only the linker-provided area, so
        // the boot CPU runs on the canonical copy itself: offset zero.
        for (auto &offset : g_cpu_offsets) {
            offset = 0;
        }

        attach_to_unit(cpu, 0);

        g_states[usize(State::Online)].reset().set(cpu);
        g_states[usize(State::Offline)].reset();
    }

    auto CpuLocal::init() -> void {
        DEBUG_ASSERT(get_init_stage() >= InitStage::Pmm,
                     "CpuLocal::init() needs the physical allocator");

        // Placement-new onto checked storage: the Gaf form of operator new is not
        // declared noexcept, so it gets no implicit null check.
        auto *mapper_storage = ktl::kmalloc(sizeof(Mapper), kGafKernel);
        auto *dyn_storage = ktl::kmalloc(sizeof(Dyn), kGafKernel);
        ASSERT(mapper_storage && dyn_storage, "Failed to allocate the cpu local bookkeeping");

        g_mapper = new (mapper_storage) Mapper();
        g_dyn = new (dyn_storage) Dyn();

        ASSERT(Status::Ok == g_mapper->init(), "Failed to lay out cpu local units");
        ASSERT(Status::Ok == g_dyn->init(*g_mapper), "Failed to init cpu local allocator");

        // Seed every unit from the canonical area so each CPU starts from the
        // initialisers the linker wrote, then publish the offsets.
        auto const &cpus = Cpu::possible_mask();
        for (auto cpu = CpuNum(0); cpu < cpus.size(); ++cpu) {
            if (!cpus.test(cpu)) {
                continue;
            }

            auto const offset = g_mapper->offset_of_unit(g_mapper->unit_of_cpu(cpu));
            auto *unit = reinterpret_cast<u8 *>(canonical_base() + offset);
            ustl::algorithms::copy_n(kInitialCpuLocalStart, initial_area_size(),
                                     reinterpret_cast<char *>(unit));
            g_cpu_offsets[cpu] = offset;

            // The copy carried the boot CPU's bookkeeping into every unit. Fix
            // it here, while `access()` still works from the outside, so a unit
            // is already self-describing before its CPU attaches to it.
            Self::access(g_current_cpu_offset, cpu) = offset;
            Self::access(g_current_cpunum, cpu) = cpu;
        }

        // The boot CPU has been running on the canonical area; move it across.
        auto const boot = Cpu::id();
        attach_to_unit(boot, g_cpu_offsets[boot]);

        bootstrap(CpuMask::of(boot));
    }

    auto CpuLocal::init_percpu(CpuNum cpu) -> void {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        DEBUG_ASSERT(Cpu::possible_mask().test(cpu), "Bringing up an impossible CPU");

        attach_to_unit(cpu, g_cpu_offsets[cpu]);

        g_states[usize(State::Online)].set(cpu);
        g_states[usize(State::Offline)].set(cpu, false);
    }

    auto CpuLocal::bootstrap(CpuMask const &cpus) -> void {
        DEBUG_ASSERT(get_init_stage() >= InitStage::Pmm, "Bootstrapping before the allocator");

        cpus.for_each([](CpuNum cpu) {
            g_states[usize(State::Online)].set(cpu);
            g_states[usize(State::Offline)].set(cpu, false);
        });
    }

    auto CpuLocal::translate(VirtAddr va) -> VirtAddr {
        // Before init() the canonical area is the only area there is.
        if (!g_mapper) {
            return is_in_initial_area(va) ? va : 0;
        }

        if (!is_in_initial_area(va) && !g_dyn->owns(va)) {
            return 0;
        }

        return va + read(g_current_cpu_offset);
    }

    auto CpuLocal::allocate_raw(usize size, usize align) -> VirtAddr {
        if (!g_dyn) {
            return 0;
        }
        return g_dyn->allocate(size, align);
    }

    auto CpuLocal::free_raw(VirtAddr canonical, usize size) -> void {
        if (!g_dyn) {
            return;
        }
        return g_dyn->deallocate(canonical, size);
    }

} // namespace ours::mm
