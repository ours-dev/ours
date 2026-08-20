#include <ours/mm/cpu-local.hpp>

#include <ours/mm/cfg.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/pm-node.hpp>
#include <ours/mm/pmm.hpp>

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
    constexpr auto const kDynCellSize = usize(sizeof(usize));

    constexpr auto const kWordBits = usize(sizeof(usize) * 8);

    /// The dynamic area is exactly one `kDynFirstChunkSize` per unit, so the
    /// bitmap is a compile-time size and needs no allocator.
    constexpr auto const kDynCells = ArchCpuLocal::kDynFirstChunkSize / kDynCellSize;
    constexpr auto const kDynWords = (kDynCells + kWordBits - 1) / kWordBits;

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

    /// Bytes the static half of a unit occupies.
    ///
    /// Rounded to the unit alignment so the dynamic area that follows starts on
    /// a page boundary. That is what lets the dynamic allocator satisfy any
    /// alignment up to a page by looking at cell indices alone.
    FORCE_INLINE
    auto static_span() -> usize {
        return align_up(initial_area_size(), ArchCpuLocal::kUnitAlign);
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
    /// A `group` is a NUMA node the units are drawn from; a `unit` is one CPU's
    /// slice of its group's allocation. Every bound here is a compile-time
    /// constant (MAX_CPU, NUMA_MAX_NODE), so the whole map is static storage --
    /// see the note on the allocator cycle above `CpuLocal::init()`.
    class CpuLocal::Mapper {
        using Self = CpuLocal::Mapper;

      public:
        auto init() -> Status;

        /// Canonical offset of `unit`, i.e. `unit_base - __cpu_local_start`.
        auto offset_of_unit(u32 unit) const -> isize {
            return m_offsets_per_unit[unit];
        }

        auto unit_of_cpu(CpuNum cpu) const -> u32 {
            return m_cpu_to_unit[cpu];
        }

        auto unit_size() const -> usize {
            return static_span() + ArchCpuLocal::kDynFirstChunkSize;
        }

        /// Canonical bounds of the dynamic area inside every unit.
        auto dyn_base() const -> VirtAddr {
            return canonical_base() + static_span();
        }

        auto dyn_size() const -> usize {
            return ArchCpuLocal::kDynFirstChunkSize;
        }

      private:
        auto create_first_chunk() -> Status;

        auto distribute_units() -> void;

        ustl::Array<u32, MAX_CPU> m_cpu_to_unit;
        ustl::Array<u32, MAX_CPU> m_unit_to_cpu;
        ustl::Array<isize, MAX_CPU> m_offsets_per_unit;

        ustl::Array<u32, NUMA_MAX_NODE> m_group_to_node;
        ustl::Array<u32, NUMA_MAX_NODE> m_num_units_per_group;
        ustl::Array<PmFrame::List, NUMA_MAX_NODE> m_group_frames;

        u32 m_num_groups;
    };

    /// Spreads the units over the nodes that have memory.
    ///
    /// This deliberately does not consult `cpu_to_node()`. The first chunk is
    /// built before the MADT is parsed, so no CPU-to-node relation exists yet;
    /// node *memory* topology, on the other hand, is already known because
    /// PmNode::bootstrap() ran first. Even placement across those nodes is the
    /// best that can be done without CPU topology. Once CPU enumeration moves
    /// ahead of the PMM this is the one function that needs rewriting.
    auto CpuLocal::Mapper::distribute_units() -> void {
        auto const &nodes = mm::node_possible_mask();

        m_num_groups = 0;
        for (auto nid = usize(0); nid < nodes.size() && m_num_groups < NUMA_MAX_NODE; ++nid) {
            if (nodes.test(nid)) {
                m_group_to_node[m_num_groups] = u32(nid);
                ++m_num_groups;
            }
        }
        if (!m_num_groups) {
            m_group_to_node[0] = 0;
            m_num_groups = 1;
        }

        // Units are identity-mapped to CPU slots and handed out in contiguous
        // runs, because a group is backed by one allocation.
        auto const per_group = u32(MAX_CPU) / m_num_groups;
        auto const remainder = u32(MAX_CPU) % m_num_groups;

        for (auto group = u32(0); group < m_num_groups; ++group) {
            m_num_units_per_group[group] = per_group + (group < remainder ? 1 : 0);
        }

        for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
            m_cpu_to_unit[unit] = unit;
            m_unit_to_cpu[unit] = unit;
        }
    }

    auto CpuLocal::Mapper::create_first_chunk() -> Status {
        // One allocation per unit rather than one per group. A group-sized block
        // would be MAX_CPU * unit_size of *contiguous* frames -- an order the
        // buddy allocator may well refuse this early -- and nothing needs units
        // to be adjacent, because every unit carries its own offset.
        auto const num_frames = align_up(unit_size(), PAGE_SIZE) / PAGE_SIZE;
        auto const order = mm::get_order(num_frames);

        for (auto group = u32(0), unit = u32(0); group < m_num_groups; ++group) {
            auto const nid = NnId(m_group_to_node[group]);

            for (auto i = u32(0); i < m_num_units_per_group[group]; ++i, ++unit) {
                auto frame = alloc_frames(nid, mm::kGafBoot, order);
                if (!frame) {
                    for (auto done = u32(0); done < m_num_groups; ++done) {
                        free_frames(&m_group_frames[done]);
                    }
                    return Status::OutOfMem;
                }

                m_group_frames[group].push_back(*frame);
                m_offsets_per_unit[unit] = isize(frame_to_virt(frame) - canonical_base());
            }
        }

        return Status::Ok;
    }

    auto CpuLocal::Mapper::init() -> Status {
        distribute_units();
        return create_first_chunk();
    }

    /// Runtime cpu local allocator.
    ///
    /// Hands out canonical addresses from the dynamic tail of every unit. One
    /// reservation covers all CPUs at once, so a single bitmap describes them.
    class CpuLocal::Dyn {
        using Self = CpuLocal::Dyn;

      public:
        auto init(Mapper &mapper) -> void;

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

        ustl::collections::intrusive::List<DynChunk> m_dynchunks;
        DynChunk m_first_chunk;
        ustl::Array<usize, kDynWords> m_first_bitmap;
    };

    auto CpuLocal::Dyn::init(Mapper &mapper) -> void {
        for (auto &word : m_first_bitmap) {
            word = 0;
        }

        m_first_chunk.bitmap = ustl::views::make_span(m_first_bitmap.data(), kDynWords);
        m_first_chunk.base = mapper.dyn_base();
        m_first_chunk.size = mapper.dyn_size();
        m_first_chunk.num_cells = mapper.dyn_size() / kDynCellSize;
        m_first_chunk.num_cells_used = 0;

        m_dynchunks.push_back(m_first_chunk);
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
        DEBUG_ASSERT(align_bytes <= ArchCpuLocal::kUnitAlign,
                     "Alignment beyond a unit boundary is not supported");

        auto const step = align_bytes / kDynCellSize;

        for (auto &chunk : m_dynchunks) {
            if (chunk.num_cells_used + cells > chunk.num_cells) {
                continue;
            }

            // The chunk base is unit-aligned, so cell k sits at a
            // `k * kDynCellSize` offset from an aligned address and stepping in
            // multiples of `step` keeps every candidate aligned.
            for (auto start = usize(0); start + cells <= chunk.num_cells; start += step) {
                auto taken = false;
                for (auto i = usize(0); i < cells; ++i) {
                    if (test(chunk, start + i)) {
                        taken = true;
                        // Resume past the blocking cell instead of rescanning it.
                        start = align_up(start + i + 1, step) - step;
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

    /// Static, because the slab cannot be used to build the thing the slab
    /// depends on. See the note above `CpuLocal::init()`.
    namespace {
        CpuLocal::Mapper g_mapper_storage;
        CpuLocal::Dyn g_dyn_storage;
    } // namespace anonymous

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
        // Before the allocator exists there is only the linker-provided area, so
        // the boot CPU runs on the canonical copy itself: offset zero.
        for (auto &offset : g_cpu_offsets) {
            offset = 0;
        }

        // Cpu::id() itself reads a CPU_LOCAL variable, so it can only be trusted
        // once a base register is installed. With offset zero it resolves to the
        // canonical copy, which is how the boot CPU learns it is CPU 0.
        attach_to_unit(0, 0);

        g_states[usize(State::Online)].reset().set(0);
        g_states[usize(State::Offline)].reset();
    }

    /// Builds the real per-CPU units.
    ///
    /// This has to run *before* the slab is initialised, and must not allocate
    /// through it, because the slab is one of its users: MemCache::init() ->
    /// CpuCache::on_mem_cache_init() -> CpuLocal::allocate<CpuCache>(). Taking
    /// even one kmalloc() here would close the loop.
    ///
    /// Everything it needs is therefore either static (the Mapper/Dyn maps,
    /// all bounded by MAX_CPU and NUMA_MAX_NODE) or comes straight from the
    /// buddy allocator (the units themselves). That leaves exactly one
    /// precondition: PmNode::bootstrap() and EarlyMem::do_handoff() have run.
    auto CpuLocal::init() -> void {
        DEBUG_ASSERT(!g_mapper, "CpuLocal::init() ran twice");

        g_mapper = &g_mapper_storage;
        ASSERT(Status::Ok == g_mapper->init(), "Failed to lay out cpu local units");

        g_dyn = &g_dyn_storage;
        g_dyn->init(*g_mapper);

        // Seed every unit from the canonical area so each CPU starts from the
        // initialisers the linker wrote, then publish the offsets.
        //
        // The copy has to happen before the boot CPU switches over: subsystems
        // that ran earlier (PmNode::Current among them) have already written
        // per-CPU state into the canonical area, and this is what carries it
        // into every unit rather than stranding it.
        for (auto cpu = CpuNum(0); cpu < MAX_CPU; ++cpu) {
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
        DEBUG_ASSERT(g_mapper, "Bringing up a CPU before CpuLocal::init()");

        attach_to_unit(cpu, g_cpu_offsets[cpu]);

        g_states[usize(State::Online)].set(cpu);
        g_states[usize(State::Offline)].set(cpu, false);
    }

    auto CpuLocal::bootstrap(CpuMask const &cpus) -> void {
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
