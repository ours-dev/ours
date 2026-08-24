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

    /// Places the per-CPU units.
    ///
    /// Everything rests on one requirement, and it is weaker than it looks:
    /// **every chunk lays its CPUs out with the same stride.** A chunk spans
    /// `MAX_CPU * unit_size` and is divided
    ///
    ///     chunk_base
    ///     |
    ///     +------------+------------+-----+------------+
    ///     | CPU 0 unit | CPU 1 unit | ... | CPU N unit |
    ///     +------------+------------+-----+------------+
    ///      \_ unit_size _/
    ///
    /// so CPU n's slice of *any* chunk is at `chunk_base + n * unit_size`. The
    /// difference `base[n] - base[0]` is therefore `n * unit_size` for every
    /// chunk alike, and one number per CPU resolves an object in any of them.
    ///
    /// What this does *not* require is that chunks sit next to each other, or
    /// next to the linker template. They can be anywhere. A handle is just
    /// `address_in_cpu0_slice - base[0]`, and `access()` adds `base[cpu]` back;
    /// the stride is what makes that work, so where the chunk itself landed
    /// never enters into it.
    ///
    /// Two ways to obtain a chunk, differing only in that:
    ///
    ///   Contiguous  one buddy allocation of `MAX_CPU * unit_size`. Needs no
    ///               address space and touches no page table, so it works on an
    ///               architecture with no window to spare. Costs a physically
    ///               contiguous block, and puts every unit on one node.
    ///
    ///   Paged       a slot of reserved address space with frames mapped into
    ///               it. No large contiguous block needed, and each unit can be
    ///               backed from its own NUMA node.
    class CpuLocal::Mapper {
        using Self = CpuLocal::Mapper;

      public:
        enum class Layout : u8 {
            Contiguous,
            Paged,
        };

        Mapper()
            : m_aspace(ARCH_CPU_LOCAL_VIRT_BASE, ARCH_CPU_LOCAL_SIZE, VmasFlags::Kernel),
              m_group_to_node(),
              m_num_groups(0),
              m_chunk0(0),
              m_next_slot(0),
              m_layout(Layout::Contiguous) {}

        /// Bytes one CPU owns in a chunk. Fixed, and identical for every chunk --
        /// it is the stride the whole scheme is built on.
        static auto unit_size() -> usize {
            return align_up(static_span() + ArchCpuLocal::kDynFirstChunkSize,
                            ArchCpuLocal::kUnitAlign);
        }

        static auto chunk_span() -> usize {
            return unit_size() * MAX_CPU;
        }

        /// Claims chunk 0 from the boot allocator.
        ///
        /// Runs before the PMM exists, so EarlyMem is the only source. The block
        /// is permanent: CPU 0 keeps the slice it takes here for good.
        auto init_early() -> Status;

        /// Chooses how *later* chunks are obtained. Chunk 0 already exists.
        auto init() -> void;

        auto ready() const -> bool {
            return m_chunk0 != 0;
        }

        auto offset_of_cpu(CpuNum cpu) const -> isize {
            return isize(m_chunk0 + usize(cpu) * unit_size() - canonical_base());
        }

        /// Handle-space address matching a chunk that starts at `chunk_base`.
        auto handle_of(VirtAddr chunk_base) const -> VirtAddr {
            return canonical_base() + (chunk_base - m_chunk0);
        }

        /// Handle-space start of the dynamic area inside chunk 0.
        auto dyn_base() const -> VirtAddr {
            return canonical_base() + static_span();
        }

        /// Obtains another chunk. Returns its base address, or 0.
        auto alloc_chunk(Gaf gaf) -> VirtAddr;

        auto can_grow() const -> bool {
            return Layout::Contiguous == m_layout || m_next_slot < kNumSlots;
        }

      private:
        constexpr static auto const kNumSlots =
            usize(ARCH_CPU_LOCAL_SIZE) / usize(ARCH_CPU_LOCAL_CHUNK_SLOT);

        auto alloc_chunk_contiguous(Gaf gaf) -> VirtAddr;

        auto alloc_chunk_paged(Gaf gaf) -> VirtAddr;

        auto distribute_groups() -> void;

        auto node_of_unit(u32 unit) const -> NnId {
            return NnId(m_group_to_node[unit % ustl::algorithms::max(m_num_groups, u32(1))]);
        }

        /// Private rather than borrowed from VmAspace::kernel_aspace(): that one
        /// is built by init_vmm(), which needs the slab, which needs us. This one
        /// costs nothing to stand up because a kernel ArchVmAspace just adopts
        /// the PGD kernel.phys already installed.
        ArchVmAspace m_aspace;

        ustl::Array<u32, NUMA_MAX_NODE> m_group_to_node;
        u32 m_num_groups;

        VirtAddr m_chunk0;
        usize m_next_slot;
        Layout m_layout;
    };

    auto CpuLocal::Mapper::init_early() -> Status {
        auto *block =
            EarlyMem::allocate<u8>(chunk_span(), ArchCpuLocal::kUnitAlign, bootmem::kAnyNode);
        if (!block) {
            return Status::OutOfMem;
        }

        m_chunk0 = VirtAddr(block);
        return Status::Ok;
    }

    /// Spreads later chunks' units over the nodes that have memory.
    ///
    /// This deliberately does not consult `cpu_to_node()`. Chunks are obtained
    /// before the MADT is parsed, so no CPU-to-node relation exists yet; node
    /// *memory* topology is already known because PmNode::bootstrap() ran first.
    auto CpuLocal::Mapper::distribute_groups() -> void {
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
    }

    auto CpuLocal::Mapper::init() -> void {
        distribute_groups();

        // Prefer paged chunks where the architecture has a window: they need no
        // large contiguous block and let each unit come from its own node. Fall
        // back rather than fail -- contiguous needs strictly less.
        if constexpr (ArchCpuLocal::kHasReservedWindow) {
            if (chunk_span() <= usize(ARCH_CPU_LOCAL_CHUNK_SLOT) &&
                Status::Ok == m_aspace.init()) {
                m_layout = Layout::Paged;
                return;
            }
        }

        m_layout = Layout::Contiguous;
    }

    auto CpuLocal::Mapper::alloc_chunk_contiguous(Gaf gaf) -> VirtAddr {
        auto const pages = align_up(chunk_span(), PAGE_SIZE) / PAGE_SIZE;
        auto const order = mm::get_order(pages);
        if (order > MAX_FRAME_ORDER) {
            return 0;
        }

        auto frame = alloc_frames(NnId(m_group_to_node[0]), gaf, order);
        return frame ? frame_to_virt(frame) : 0;
    }

    auto CpuLocal::Mapper::alloc_chunk_paged(Gaf gaf) -> VirtAddr {
        if (m_next_slot >= kNumSlots) {
            return 0;
        }

        auto const slot = VirtAddr(ARCH_CPU_LOCAL_VIRT_BASE) +
                          m_next_slot * usize(ARCH_CPU_LOCAL_CHUNK_SLOT);
        auto const pages = align_up(unit_size(), PAGE_SIZE) / PAGE_SIZE;

        for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
            auto const unit_va = slot + usize(unit) * unit_size();

            // Exactly `pages`, walked with descending power-of-two blocks.
            // Rounding up to one order would map past the unit and collide with
            // the next one, since unit_size need not be a power of two pages.
            for (auto done = usize(0); done < pages;) {
                auto const remaining = pages - done;
                auto const order = ustl::algorithms::min(usize(ustl::bit_width(remaining)) - 1,
                                                         usize(MAX_FRAME_ORDER));
                auto const n = usize(BIT(order));

                auto frame = alloc_frames(node_of_unit(unit), gaf, order);
                if (!frame) {
                    // Earlier units stay mapped. The slot is abandoned rather
                    // than unwound; the area is never torn down anyway.
                    return 0;
                }

                auto mapped = usize(0);
                auto const status =
                    m_aspace.map(unit_va + done * PAGE_SIZE, frame_to_phys(frame), n, kMapFlags,
                                 MapControl::ErrorIfExisting, &mapped);
                if (Status::Ok != status) {
                    return 0;
                }

                done += n;
            }
        }

        ++m_next_slot;
        return slot;
    }

    auto CpuLocal::Mapper::alloc_chunk(Gaf gaf) -> VirtAddr {
        return Layout::Paged == m_layout ? alloc_chunk_paged(gaf) : alloc_chunk_contiguous(gaf);
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
    auto CpuLocal::init_early() -> void {
        g_mapper = &g_mapper_storage;
        ASSERT(Status::Ok == g_mapper->init_early(), "Failed to claim the cpu local first chunk");

        for (auto &offset : g_cpu_offsets) {
            offset = 0;
        }

        // The boot CPU is CPU 0 by construction, so there is nothing to look up
        // -- which also avoids asking Cpu::id(), whose answer lives in the very
        // storage being set up here.
        auto const boot = CpuNum(0);
        auto const offset = g_mapper->offset_of_cpu(boot);

        ustl::algorithms::copy_n(kInitialCpuLocalStart, initial_area_size(),
                                 reinterpret_cast<char *>(canonical_base() + offset));

        g_cpu_offsets[boot] = offset;
        attach_to_unit(boot, offset);

        g_states[usize(State::Online)].reset().set(boot);
        g_states[usize(State::Offline)].reset();
    }

    /// Finishes what init_early() started, once the PMM exists.
    ///
    /// This has to run *before* the slab is initialised, and must not allocate
    /// through it, because the slab is one of its users: MemCache::init() ->
    /// CpuCache::on_mem_cache_init() -> CpuLocal::allocate<CpuCache>(). Taking
    /// even one kmalloc() here would close the loop.
    auto CpuLocal::init() -> void {
        DEBUG_ASSERT(g_mapper && g_mapper->ready(), "CpuLocal::init_early() has not run");
        DEBUG_ASSERT(!g_dyn, "CpuLocal::init() ran twice");

        // Decide how later chunks will be obtained now that there is a PMM.
        g_mapper->init();

        // Seed the remaining units from the template. CPU 0 is deliberately left
        // alone: it has been running on its slice since init_early() and its
        // state must survive, whereas the others must start from the defaults.
        for (auto cpu = CpuNum(1); cpu < MAX_CPU; ++cpu) {
            auto const offset = g_mapper->offset_of_cpu(cpu);
            ustl::algorithms::copy_n(kInitialCpuLocalStart, initial_area_size(),
                                     reinterpret_cast<char *>(canonical_base() + offset));
            g_cpu_offsets[cpu] = offset;

            // Fix each unit's own bookkeeping while access() still works from the
            // outside, so a unit is self-describing before its CPU attaches.
            Self::access(g_current_cpu_offset, cpu) = offset;
            Self::access(g_current_cpunum, cpu) = cpu;
        }

        g_dyn = &g_dyn_storage;
        g_dyn->init(*g_mapper);
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
