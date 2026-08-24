#include <ours/mm/cpu-local.hpp>

#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/arch-vm-aspace.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/early-mem.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/pm-node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vmas-flags.hpp>
#include <ours/sync/spin-lock.hpp>

#include <ustl/algorithms/copy.hpp>
#include <ustl/algorithms/minmax.hpp>
#include <ustl/bit.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/views/span.hpp>

/// An architecture whose ArchCpuLocal reports `kHasReservedWindow == false` has
/// no window to describe, and should not have to invent one to compile this
/// file. The paged chunk allocator is never selected there, so these are never
/// read.
#ifndef ARCH_CPU_LOCAL_VIRT_BASE
#   define ARCH_CPU_LOCAL_VIRT_BASE 0
#   define ARCH_CPU_LOCAL_CHUNK_SLOT PAGE_SIZE
#   define ARCH_CPU_LOCAL_SIZE 0
#endif

using ustl::mm::align_up;

extern char const kInitialCpuLocalStart[] LINK_NAME("__cpu_local_start");
extern char const kInitialCpuLocalEnd[] LINK_NAME("__cpu_local_end");

namespace ours::mm {
namespace {
    /// Allocation granularity of the dynamic area. Every reservation is a whole
    /// number of cells so the bitmap has one bit per cell.
    constexpr auto const kDynCellSize = usize(sizeof(usize));

    constexpr auto const kWordBits = usize(sizeof(usize) * 8);

    /// Chunk 0's dynamic area is exactly `kDynFirstChunkSize` per CPU (see
    /// Mapper::unit_size), so its bitmap is a compile-time size and needs no
    /// allocator at all.
    constexpr auto const kDynCells = ArchCpuLocal::kDynFirstChunkSize / kDynCellSize;
    constexpr auto const kDynWords = (kDynCells + kWordBits - 1) / kWordBits;

    constexpr auto const kMapFlags = MmuFlags::Present | MmuFlags::Readable | MmuFlags::Writable;

    /// Growth runs under Dyn's spin lock, i.e. with preemption off, so it must
    /// not take a path that can reclaim or wait. kGafBoot and kGafKernel both
    /// carry DirectReclaim; this one refuses instead, and a refusal only means
    /// the allocation fails.
    constexpr auto const kGafGrow = Gaf::ThisNode | Gaf::Zero | Gaf::NoRetry;

    FORCE_INLINE
    auto initial_area_size() -> usize {
        return kInitialCpuLocalEnd - kInitialCpuLocalStart;
    }

    /// Origin of handle space: the address the linker gave the template.
    ///
    /// Nothing is ever read or written through it after init_early(). It is the
    /// zero point that `access()` measures from, no more -- which matters,
    /// because the template itself lives in the `.init` area and is handed back
    /// to the allocator by reclaim_init_area().
    FORCE_INLINE
    auto canonical_base() -> VirtAddr {
        return VirtAddr(kInitialCpuLocalStart);
    }

    FORCE_INLINE
    auto is_in_initial_area(VirtAddr va) -> bool {
        return va >= VirtAddr(kInitialCpuLocalStart) && va < VirtAddr(kInitialCpuLocalEnd);
    }

    /// Bytes the static half of a unit occupies, rounded so the dynamic area
    /// behind it starts on a page boundary.
    FORCE_INLINE
    auto static_span() -> usize {
        return align_up(initial_area_size(), ArchCpuLocal::kUnitAlign);
    }
} // namespace anonymous

    struct DynChunk: ustl::collections::intrusive::ListBaseHook<> {
        /// Handle-space address of the first byte this chunk manages.
        VirtAddr base;
        usize size;

        /// One bit per `kDynCellSize` bytes; set means taken.
        ustl::views::Span<usize> bitmap;

        usize num_cells;
        usize num_cells_used;
    };

    /// Places the per-CPU units and keeps the map from CPU to unit.
    ///
    /// Handle space is the "dummy region" starting at `__cpu_local_start`: an
    /// address nothing is ever read or written through, existing only so an
    /// object has one name that means the same thing on every CPU. A handle is
    /// `real_address - (m_base - __cpu_local_start)`, and `access()` adds
    /// `g_cpu_offsets[cpu]` back.
    ///
    /// Units are grouped by NUMA node: one allocation per node, units of the
    /// CPUs on that node laid contiguously inside it. Nodes are unrelated to
    /// each other, so `m_unit_offset` is a table rather than a stride --
    ///
    ///     m_base
    ///     |
    ///     +-- node 0 block --------+      +-- node 1 block ---------+
    ///     | unit(c0) | unit(c1)    |      | unit(c2) | unit(c3)     |
    ///     +------------------------+      +-------------------------+
    ///      \_unit_size_/                   \_unit_size_/
    ///
    /// -- and the offsets need not be related at all. What growth *does* need is
    /// that a later chunk reproduce the same table against its own base: an
    /// object at `B + m_unit_offset[n] + d` has handle
    /// `B - m_base + __cpu_local_start + d`, which drops the `n` and so names it
    /// on every CPU at once. Reserved address space makes that easy; without it
    /// the table has to fit inside one contiguous block, which only holds when
    /// there is a single group.
    class CpuLocal::Mapper {
        using Self = CpuLocal::Mapper;

      public:
        Mapper()
            : m_aspace(ARCH_CPU_LOCAL_VIRT_BASE, ARCH_CPU_LOCAL_SIZE, VmasFlags::Kernel),
              m_c2u(), m_u2c(), m_n2g(), m_g2n(), m_unit_offset(),
              m_group_num_units(), m_group_frames(), m_base(0), m_replica(0), m_num_units(0),
              m_num_groups(0), m_next_slot(0), m_paged(false) {}

        /// One unit's worth: the static copy plus the first dynamic chunk.
        ///
        /// Rounded to a whole buddy order so the boot replica can be handed back
        /// exactly, rather than by an order that covers more than was taken.
        static auto unit_size() -> usize {
            auto const want = align_up(static_span() + ArchCpuLocal::kDynFirstChunkSize,
                                       ArchCpuLocal::kUnitAlign);
            return usize(BIT(mm::get_order(want / PAGE_SIZE))) * PAGE_SIZE;
        }

        /// Claims the boot CPU's provisional unit from the boot allocator.
        ///
        /// One unit, for CPU 0, and nothing else: the other CPUs are not even
        /// enumerated this early. It is provisional because init() will place
        /// CPU 0 on a NUMA-correct unit alongside everyone else and hand this
        /// block back.
        auto init_early() -> Status;

        /// Builds the real areas: node groups, unit offsets, backing frames.
        auto init() -> Status;

        auto replica() const -> VirtAddr {
            return m_replica;
        }

        auto num_units() const -> u32 {
            return m_num_units;
        }

        auto unit_to_cpu(u32 unit) const -> CpuNum {
            return CpuNum(m_u2c[unit]);
        }

        auto unit_base(u32 unit) const -> VirtAddr {
            return m_base + usize(m_unit_offset[unit]);
        }

        /// `g_cpu_offsets[cpu]` for a unit -- what `access()` adds.
        auto offset_of_unit(u32 unit) const -> isize {
            return isize(unit_base(unit) - canonical_base());
        }

        /// Real address to handle. The legacy `fix_addr`.
        auto handle_of(VirtAddr real) const -> VirtAddr {
            return real - (m_base - canonical_base());
        }

        /// Handle-space start of the dynamic area inside every unit.
        static auto dyn_base() -> VirtAddr {
            return canonical_base() + static_span();
        }

        /// Reproduces the unit table over a fresh span. Returns the new chunk's
        /// base address, or 0.
        auto alloc_chunk(Gaf gaf) -> VirtAddr;

        auto can_grow() const -> bool;

      private:
        constexpr static auto const kNumSlots =
            usize(ARCH_CPU_LOCAL_SIZE) / usize(ARCH_CPU_LOCAL_CHUNK_SLOT);

        auto build_map_info() -> void;

        auto build_area_info() -> Status;

        /// Bytes from the lowest unit to the end of the highest -- what a chunk
        /// must span to hold the whole table.
        auto table_span() const -> usize;

        ArchVmAspace m_aspace;

        ustl::Array<i32, MAX_CPU> m_c2u;          // CPU   -> unit
        ustl::Array<i32, MAX_CPU> m_u2c;          // unit  -> CPU
        ustl::Array<i32, NUMA_MAX_NODE> m_n2g;    // node  -> group
        ustl::Array<i32, NUMA_MAX_NODE> m_g2n;    // group -> node
        ustl::Array<isize, MAX_CPU> m_unit_offset;
        ustl::Array<u32, NUMA_MAX_NODE> m_group_num_units;
        ustl::Array<PmFrame::List, NUMA_MAX_NODE> m_group_frames;

        VirtAddr m_base;
        VirtAddr m_replica;
        u32 m_num_units;
        u32 m_num_groups;
        usize m_next_slot;
        bool m_paged;
    };

    auto CpuLocal::Mapper::init_early() -> Status {
        auto *block = EarlyMem::allocate<u8>(unit_size(), ArchCpuLocal::kUnitAlign,
                                             bootmem::kAnyNode);
        if (!block) {
            return Status::OutOfMem;
        }

        m_replica = VirtAddr(block);
        return Status::Ok;
    }

    /// Numbers nodes and CPUs densely, keeping every group's units contiguous so
    /// one allocation per node can back all of them.
    ///
    /// A CPU that no node claims still needs a unit -- a missing or broken SRAT
    /// must not leave one unmapped -- so those trail the nodes in their own
    /// group.
    auto CpuLocal::Mapper::build_map_info() -> void {
        auto const &cpus = Cpu::possible_mask();
        auto const &nodes = mm::node_possible_mask();

        for (auto &unit : m_c2u) {
            unit = -1;
        }

        auto unit = u32(0);
        auto group = u32(0);
        for (auto nid = usize(0); nid < nodes.size(); ++nid) {
            if (!nodes.test(nid)) {
                continue;
            }

            auto const *affinity = mm::node_hard_affinity(NnId(nid));
            if (!affinity) {
                continue;
            }

            auto here = u32(0);
            for (auto cpu = CpuNum(0); cpu < cpus.size(); ++cpu) {
                if (!cpus.test(cpu) || !affinity->test(cpu) || m_c2u[cpu] >= 0) {
                    continue;
                }

                m_c2u[cpu] = i32(unit);
                m_u2c[unit] = i32(cpu);
                ++unit;
                ++here;
            }

            if (!here) {
                continue;
            }

            m_n2g[nid] = i32(group);
            m_g2n[group] = i32(nid);
            m_group_num_units[group] = here;
            ++group;
        }

        auto orphans = u32(0);
        for (auto cpu = CpuNum(0); cpu < cpus.size(); ++cpu) {
            if (!cpus.test(cpu) || m_c2u[cpu] >= 0) {
                continue;
            }

            m_c2u[cpu] = i32(unit);
            m_u2c[unit] = i32(cpu);
            ++unit;
            ++orphans;
        }

        if (orphans) {
            DEBUG_ASSERT(group < NUMA_MAX_NODE, "No group left for unclaimed CPUs");
            m_g2n[group] = 0;
            m_group_num_units[group] = orphans;
            ++group;
        }

        m_num_units = unit;
        m_num_groups = group;
    }

    auto CpuLocal::Mapper::build_area_info() -> Status {
        auto const size = unit_size();

        m_base = ustl::NumericLimits<VirtAddr>::max();
        for (auto group = u32(0); group < m_num_groups; ++group) {
            auto const nid = NnId(m_g2n[group]);
            auto const nr_units = m_group_num_units[group];
            auto const order = mm::get_order(align_up(size * nr_units, PAGE_SIZE) / PAGE_SIZE);

            // kGafBoot implies Gaf::Zero, so a unit's dynamic area starts clean.
            auto frame = alloc_frames(nid, mm::kGafBoot, order);
            if (!frame) {
                for (auto done = u32(0); done < group; ++done) {
                    free_frames(&m_group_frames[done]);
                }
                return Status::OutOfMem;
            }

            m_group_frames[group].push_back(*frame);
            m_base = ustl::algorithms::min(m_base, frame_to_virt(frame));
        }

        for (auto group = u32(0), unit = u32(0); group < m_num_groups; ++group) {
            auto const addr = frame_to_virt(&m_group_frames[group].front());

            for (auto i = u32(0); i < m_group_num_units[group]; ++i, ++unit) {
                m_unit_offset[unit] = isize(addr + size * i - m_base);
            }
        }

        return Status::Ok;
    }

    auto CpuLocal::Mapper::init() -> Status {
        build_map_info();
        if (!m_num_units) {
            return Status::InvalidArguments;
        }

        auto const status = build_area_info();
        if (Status::Ok != status) {
            return status;
        }

        // Later chunks have to reproduce the unit table, which is easy against
        // reserved address space and otherwise needs the whole table to fit one
        // contiguous block.
        if constexpr (ArchCpuLocal::kHasReservedWindow) {
            m_paged = table_span() <= usize(ARCH_CPU_LOCAL_CHUNK_SLOT) &&
                      Status::Ok == m_aspace.init();
        }

        return Status::Ok;
    }

    auto CpuLocal::Mapper::table_span() const -> usize {
        auto highest = usize(0);
        for (auto unit = u32(0); unit < m_num_units; ++unit) {
            highest = ustl::algorithms::max(highest, usize(m_unit_offset[unit]) + unit_size());
        }
        return highest;
    }

    auto CpuLocal::Mapper::can_grow() const -> bool {
        if (m_paged) {
            return m_next_slot < kNumSlots;
        }

        // Without address space a chunk is one contiguous block, so the table
        // has to be dense -- which it only is when every unit came from the
        // same group.
        return m_num_groups == 1 &&
               mm::get_order(align_up(table_span(), PAGE_SIZE) / PAGE_SIZE) <= MAX_FRAME_ORDER;
    }

    auto CpuLocal::Mapper::alloc_chunk(Gaf gaf) -> VirtAddr {
        auto const size = unit_size();
        auto const span = table_span();

        if (!m_paged) {
            auto const order = mm::get_order(align_up(span, PAGE_SIZE) / PAGE_SIZE);
            if (order > MAX_FRAME_ORDER) {
                return 0;
            }
            auto frame = alloc_frames(NnId(m_g2n[0]), gaf, order);
            return frame ? frame_to_virt(frame) : 0;
        }

        if (m_next_slot >= kNumSlots) {
            return 0;
        }

        auto const slot = VirtAddr(ARCH_CPU_LOCAL_VIRT_BASE) +
                          m_next_slot * usize(ARCH_CPU_LOCAL_CHUNK_SLOT);
        auto const pages = size / PAGE_SIZE;

        // One unit at a time, at the offset the table dictates, and from the
        // node that unit belongs to.
        for (auto group = u32(0), unit = u32(0); group < m_num_groups; ++group) {
            auto const nid = NnId(m_g2n[group]);

            for (auto i = u32(0); i < m_group_num_units[group]; ++i, ++unit) {
                auto const va = slot + usize(m_unit_offset[unit]);

                for (auto done = usize(0); done < pages;) {
                    auto const remaining = pages - done;
                    auto const order = ustl::algorithms::min(
                        usize(ustl::bit_width(remaining)) - 1, usize(MAX_FRAME_ORDER));
                    auto const n = usize(BIT(order));

                    auto frame = alloc_frames(nid, gaf, order);
                    if (!frame) {
                        // Earlier units stay mapped; the slot is abandoned
                        // rather than unwound, as nothing here is ever freed.
                        return 0;
                    }

                    auto mapped = usize(0);
                    if (Status::Ok != m_aspace.map(va + done * PAGE_SIZE, frame_to_phys(frame), n,
                                                   kMapFlags, MapControl::ErrorIfExisting,
                                                   &mapped)) {
                        return 0;
                    }

                    done += n;
                }
            }
        }

        ++m_next_slot;
        return slot;
    }

    /// Runtime cpu local allocator.
    ///
    /// Hands out handles, not pointers. One reservation covers all CPUs at once
    /// -- it is the same slice of every unit -- so a single bitmap describes
    /// them all and the allocator never needs to know how many CPUs there are.
    class CpuLocal::Dyn {
        using Self = CpuLocal::Dyn;

      public:
        auto init(Mapper &mapper) -> void;

        auto allocate(usize size, usize align) -> VirtAddr;

        auto deallocate(VirtAddr handle, usize size) -> void;

        auto owns(VirtAddr handle) const -> bool;

      private:
        auto grow() -> bool;

        auto allocate_locked(usize cells, usize align_bytes) -> VirtAddr;

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

        mutable sync::SpinLock m_lock;
        Mapper *m_mapper;
        ustl::collections::intrusive::List<DynChunk> m_dynchunks;
        DynChunk m_first_chunk;
        ustl::Array<usize, kDynWords> m_first_bitmap;
    };

    auto CpuLocal::Dyn::init(Mapper &mapper) -> void {
        m_mapper = &mapper;

        for (auto &word : m_first_bitmap) {
            word = 0;
        }

        // Chunk 0 is the one whose unit carries the static copy, so its dynamic
        // part is what is left over -- exactly kDynFirstChunkSize, since
        // static_span() is already a multiple of kUnitAlign.
        m_first_chunk.base = mapper.dyn_base();
        m_first_chunk.size = ArchCpuLocal::kDynFirstChunkSize;
        m_first_chunk.bitmap = ustl::views::make_span(m_first_bitmap.data(), kDynWords);
        m_first_chunk.num_cells = kDynCells;
        m_first_chunk.num_cells_used = 0;

        m_dynchunks.push_back(m_first_chunk);
    }

    /// Takes another chunk and puts a DynChunk over it.
    ///
    /// Later chunks carry no static copy, so the whole unit is dynamic. The
    /// chunk header and its bitmap share one buddy page rather than coming from
    /// the slab: CpuLocal must not depend on an allocator it is a prerequisite
    /// of, and that stays true after boot, not just during it.
    auto CpuLocal::Dyn::grow() -> bool {
        if (!m_mapper->can_grow()) {
            return false;
        }

        auto const base = m_mapper->alloc_chunk(kGafGrow);
        if (!base) {
            return false;
        }

        auto frame = alloc_frames(kGafGrow, usize(0));
        if (!frame) {
            return false;
        }

        auto *page = reinterpret_cast<u8 *>(frame_to_virt(frame));
        auto const header = align_up(sizeof(DynChunk), alignof(usize));
        auto const words = (PAGE_SIZE - header) / sizeof(usize);

        auto *chunk = new (static_cast<void *>(page)) DynChunk();
        auto *bits = reinterpret_cast<usize *>(page + header);

        chunk->base = m_mapper->handle_of(base);
        chunk->size = Mapper::unit_size();
        chunk->num_cells = chunk->size / kDynCellSize;
        chunk->num_cells_used = 0;

        // A unit whose bitmap would not fit in the page it shares is a
        // misconfiguration, not a runtime condition.
        DEBUG_ASSERT(chunk->num_cells <= words * kWordBits, "Unit size outruns its bitmap");
        chunk->bitmap = ustl::views::make_span(bits, words);

        m_dynchunks.push_back(*chunk);
        return true;
    }

    auto CpuLocal::Dyn::owns(VirtAddr handle) const -> bool {
        Guard guard{m_lock};

        for (auto const &chunk : m_dynchunks) {
            if (handle >= chunk.base && handle < chunk.base + chunk.size) {
                return true;
            }
        }
        return false;
    }

    auto CpuLocal::Dyn::allocate_locked(usize cells, usize align_bytes) -> VirtAddr {
        auto const step = align_bytes / kDynCellSize;

        for (auto &chunk : m_dynchunks) {
            if (chunk.num_cells_used + cells > chunk.num_cells) {
                continue;
            }

            // Alignment is a property of the address, not of the cell index, so
            // anchor the scan on the first cell whose address qualifies and step
            // in whole multiples of the alignment from there.
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

    auto CpuLocal::Dyn::allocate(usize size, usize align) -> VirtAddr {
        if (!size) {
            return 0;
        }

        auto const cells = align_up(size, kDynCellSize) / kDynCellSize;
        auto const align_bytes = ustl::algorithms::max(align, kDynCellSize);
        DEBUG_ASSERT(!(align_bytes & (align_bytes - 1)), "Alignment must be a power of two");
        DEBUG_ASSERT(align_bytes <= ArchCpuLocal::kUnitAlign,
                     "Alignment beyond a unit boundary is not supported");

        Guard guard{m_lock};

        if (auto const found = allocate_locked(cells, align_bytes)) {
            return found;
        }
        if (!grow()) {
            return 0;
        }
        return allocate_locked(cells, align_bytes);
    }

    auto CpuLocal::Dyn::deallocate(VirtAddr handle, usize size) -> void {
        if (!handle || !size) {
            return;
        }

        auto const cells = align_up(size, kDynCellSize) / kDynCellSize;

        Guard guard{m_lock};

        for (auto &chunk : m_dynchunks) {
            if (handle < chunk.base || handle >= chunk.base + chunk.size) {
                continue;
            }

            auto const start = (handle - chunk.base) / kDynCellSize;
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

    /// Zero rather than INVALID_CPU_NUM: the template is what CPU 0 reads until
    /// init_early() moves it onto a real unit, and the boot CPU is CPU 0.
    CPU_LOCAL
    decltype(CpuLocal::g_current_cpunum) CpuLocal::g_current_cpunum = 0;

    CPU_LOCAL
    decltype(CpuLocal::g_current_cpu_offset) CpuLocal::g_current_cpu_offset = 0;

    /// Static, because the slab cannot be used to build what the slab depends on.
    namespace {
        CpuLocal::Mapper g_mapper_storage;
        CpuLocal::Dyn g_dyn_storage;
    } // namespace anonymous

    CpuLocal::Mapper *CpuLocal::g_mapper;
    CpuLocal::Dyn *CpuLocal::g_dyn;

    /// Order matters: the base register has to be live before any `read`/`write`
    /// can reach the unit, so it is installed first and the bookkeeping written
    /// through it afterwards. Writing first would land in the template, which is
    /// exactly what must not be written.
    auto CpuLocal::attach_to_unit(CpuNum cpunum, isize offset) -> void {
        ArchCpuLocal::install(offset);
        Self::write(g_current_cpu_offset, offset);
        Self::write(g_current_cpunum, cpunum);
    }

    /// Gets the boot CPU onto a unit of its own, as early as possible.
    ///
    /// It cannot simply run on the template at offset zero. That memory belongs
    /// to the `.init` area and is handed back to the allocator by
    /// reclaim_init_area(); it is a read-only image of the defaults, not
    /// storage. So chunk 0 is claimed from the boot allocator here and CPU 0 is
    /// seeded from the template and moved onto it straight away.
    ///
    /// Only CPU 0. The other units in the chunk stay untouched until init()
    /// seeds them -- from the template, so that they get pristine defaults
    /// rather than whatever CPU 0 has been accumulating in the meantime.
    /// Gets the boot CPU onto storage of its own, as early as possible.
    ///
    /// It cannot run on the linker template. That is a read-only image of the
    /// defaults, and it lives in the `.init` area that reclaim_init_area() hands
    /// back to the allocator -- it is a source to copy from, never storage. So a
    /// provisional unit is taken from the boot allocator here, seeded, and moved
    /// onto immediately.
    ///
    /// Only CPU 0; no other CPU is even enumerated yet.
    auto CpuLocal::init_early() -> void {
        g_mapper = &g_mapper_storage;
        ASSERT(Status::Ok == g_mapper->init_early(), "Failed to claim the boot cpu local unit");

        for (auto &offset : g_cpu_offsets) {
            offset = 0;
        }

        // The boot CPU is CPU 0 by construction, so there is nothing to look up
        // -- which also avoids asking Cpu::id(), whose answer lives in the very
        // storage being set up here.
        auto const boot = CpuNum(0);
        auto const replica = g_mapper->replica();
        auto const offset = isize(replica - canonical_base());

        ustl::algorithms::copy_n(kInitialCpuLocalStart, initial_area_size(),
                                 reinterpret_cast<char *>(replica));

        g_cpu_offsets[boot] = offset;
        attach_to_unit(boot, offset);

        // The first dynamic chunk lives in the tail of this unit. It is recorded
        // in handle space, so it survives the migration in init() untouched --
        // the same handle simply resolves against a different unit base.
        g_dyn = &g_dyn_storage;
        g_dyn->init(*g_mapper);

        g_states[usize(State::Online)].reset().set(boot);
        g_states[usize(State::Offline)].reset();
    }

    /// Builds the real per-CPU areas, once the PMM exists.
    ///
    /// This has to run *before* the slab is initialised, and must not allocate
    /// through it, because the slab is one of its users: MemCache::init() ->
    /// CpuCache::on_mem_cache_init() -> CpuLocal::allocate<CpuCache>(). Taking
    /// even one kmalloc() here would close the loop.
    auto CpuLocal::init() -> void {
        DEBUG_ASSERT(g_mapper && g_mapper->replica(), "CpuLocal::init_early() has not run");

        ASSERT(Status::Ok == g_mapper->init(), "Failed to lay out cpu local units");

        auto const replica = g_mapper->replica();
        auto const nr_units = g_mapper->num_units();

        for (auto unit = u32(0); unit < nr_units; ++unit) {
            auto const cpu = g_mapper->unit_to_cpu(unit);
            auto const offset = g_mapper->offset_of_unit(unit);
            auto *base = reinterpret_cast<u8 *>(g_mapper->unit_base(unit));

            if (cpu == 0) {
                // CPU 0 has been running on the provisional unit since
                // init_early(); carry what it accumulated across, dynamic tail
                // included, rather than resetting it to the defaults.
                ustl::algorithms::copy_n(reinterpret_cast<u8 const *>(replica),
                                         Mapper::unit_size(), base);
            } else {
                // Everyone else starts from the template. Copying CPU 0's unit
                // here instead would hand every CPU CPU 0's PmNode::Current and
                // apic_id.
                ustl::algorithms::copy_n(kInitialCpuLocalStart, initial_area_size(),
                                         reinterpret_cast<char *>(base));
            }

            g_cpu_offsets[cpu] = offset;
        }

        // Only now that every offset is published can access() reach a unit from
        // the outside; fix each one's own bookkeeping so it is self-describing
        // before its CPU attaches.
        for (auto unit = u32(0); unit < nr_units; ++unit) {
            auto const cpu = g_mapper->unit_to_cpu(unit);
            Self::access(g_current_cpu_offset, cpu) = g_cpu_offsets[cpu];
            Self::access(g_current_cpunum, cpu) = cpu;
        }

        // Move CPU 0 onto its real unit and release the provisional one. The
        // unit size is a whole buddy order, so this hands back exactly what was
        // taken.
        attach_to_unit(0, g_cpu_offsets[0]);
        free_frames(replica, mm::get_order(Mapper::unit_size() / PAGE_SIZE));

        bootstrap(CpuMask::of(0));
    }

    auto CpuLocal::init_percpu(CpuNum cpu) -> void {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        DEBUG_ASSERT(g_dyn, "Bringing up a CPU before CpuLocal::init()");

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
        if (!is_in_initial_area(va) && (!g_dyn || !g_dyn->owns(va))) {
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

    auto CpuLocal::free_raw(VirtAddr handle, usize size) -> void {
        if (!g_dyn) {
            return;
        }
        return g_dyn->deallocate(handle, size);
    }

} // namespace ours::mm
