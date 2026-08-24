#include <ours/mm/cpu-local.hpp>

#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/arch-vm-aspace.hpp>
#include <ours/mm/cfg.hpp>
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
/// file. The paged layout is never selected there, so these are never read.
#ifndef ARCH_CPU_LOCAL_VIRT_BASE
#   define ARCH_CPU_LOCAL_VIRT_BASE 0
#   define ARCH_CPU_LOCAL_SIZE 0
#   define ARCH_CPU_LOCAL_UNIT_STRIDE PAGE_SIZE
#endif

using ustl::mm::align_up;

extern char const kInitialCpuLocalStart[] LINK_NAME("__cpu_local_start");
extern char const kInitialCpuLocalEnd[] LINK_NAME("__cpu_local_end");

namespace ours::mm {
namespace {
    /// Address space reserved per CPU. Only the populated prefix has frames
    /// behind it; the rest is there so the dynamic area can grow in place.
    constexpr auto const kUnitStride = usize(ARCH_CPU_LOCAL_UNIT_STRIDE);

    /// Allocation granularity of the dynamic area. Every reservation is a whole
    /// number of cells so the bitmap has one bit per cell.
    constexpr auto const kDynCellSize = usize(sizeof(usize));

    constexpr auto const kWordBits = usize(sizeof(usize) * 8);

    /// The first chunk is exactly `kDynFirstChunkSize`, so its bitmap is a
    /// compile-time size and needs no allocator at all.
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
        /// Canonical address of the first byte this chunk manages.
        VirtAddr base;
        usize size;

        /// One bit per `kDynCellSize` bytes; set means taken.
        ustl::views::Span<usize> bitmap;

        usize num_cells;
        usize num_cells_used;
    };

    /// Places the per-CPU units and reports where they landed.
    ///
    /// Both layouts below produce the same thing -- a per-unit base whose
    /// interior is `[static copy | dynamic area]` -- because that is what the
    /// single-offset invariant needs:
    ///
    ///     unit_base(n)                          +static_span   +dyn_limit
    ///     |                                              |            |
    ///     +----------------------------------------------+------------+
    ///     | copy of [__cpu_local_start, __cpu_local_end) | dynamic    |
    ///     +----------------------------------------------+------------+
    ///
    /// An object at `static_span + k` is that same distance from *every* CPU's
    /// static block, so one canonical address names it everywhere. Where the two
    /// layouts differ is what sits behind the unit, and therefore whether the
    /// dynamic area can ever get bigger.
    ///
    /// **Embedded** (`kHasReservedWindow == false`). Each unit is a single buddy
    /// allocation living in the physmap, at an address unrelated to its
    /// neighbours. Costs no address space, so it suits an architecture that
    /// cannot spare a large contiguous window -- a 32-bit kernel, say. The
    /// dynamic area is fixed at `kDynFirstChunkSize`: a second block would sit
    /// at a *different* distance from the static copy on each CPU, and no single
    /// offset could describe it.
    ///
    /// **Paged** (`kHasReservedWindow == true`). Every unit gets `kUnitStride` of
    /// reserved virtual address space and only as many frames as it needs.
    /// Growing maps more frames into space already reserved, so the distance from
    /// the static copy is unchanged and nothing already handed out moves.
    class CpuLocal::Mapper {
        using Self = CpuLocal::Mapper;

      public:
        enum class Layout : u8 {
            Embedded,
            Paged,
        };

        Mapper()
            : m_aspace(ARCH_CPU_LOCAL_VIRT_BASE, ARCH_CPU_LOCAL_SIZE, VmasFlags::Kernel),
              m_cpu_to_unit(),
              m_unit_to_group(),
              m_offsets_per_unit(),
              m_group_to_node(),
              m_num_groups(0),
              m_populated(0),
              m_layout(Layout::Embedded) {}

        auto init() -> Status;

        auto layout() const -> Layout {
            return m_layout;
        }

        /// Canonical offset of `unit`, i.e. `unit_base - __cpu_local_start`.
        ///
        /// Stored per unit rather than derived from a stride, because under the
        /// embedded layout there is no stride to derive it from.
        auto offset_of_unit(u32 unit) const -> isize {
            return m_offsets_per_unit[unit];
        }

        auto unit_of_cpu(CpuNum cpu) const -> u32 {
            return m_cpu_to_unit[cpu];
        }

        /// Canonical start of the dynamic area inside every unit.
        auto dyn_base() const -> VirtAddr {
            return canonical_base() + static_span();
        }

        /// Canonical address the dynamic area can never pass.
        auto dyn_limit() const -> VirtAddr {
            return Layout::Paged == m_layout
                       ? canonical_base() + kUnitStride
                       : dyn_base() + ArchCpuLocal::kDynFirstChunkSize;
        }

        /// Bytes of every unit that currently have frames behind them.
        auto populated() const -> usize {
            return m_populated;
        }

        /// Whether the dynamic area can still be extended.
        auto can_grow() const -> bool {
            return Layout::Paged == m_layout && canonical_base() + m_populated < dyn_limit();
        }

        /// Backs `[populated(), target)` of every unit with frames.
        ///
        /// Paged layout only -- the embedded one has nowhere to put the frames.
        /// `target` is met exactly (after page rounding) rather than rounded up
        /// to one buddy order: the chunk that will cover the new space is sized
        /// from `target`, and any surplus past it would be populated but
        /// unreachable.
        auto populate(usize target, Gaf gaf) -> Status;

      private:
        auto init_embedded() -> Status;

        auto init_paged() -> Status;

        auto distribute_units() -> void;

        static auto paged_unit_base(u32 unit) -> VirtAddr {
            return VirtAddr(ARCH_CPU_LOCAL_VIRT_BASE) + usize(unit) * kUnitStride;
        }

        auto node_of_unit(u32 unit) const -> NnId {
            return NnId(m_group_to_node[m_unit_to_group[unit]]);
        }

        /// Private rather than borrowed from VmAspace::kernel_aspace(): that one
        /// is built by init_vmm(), which needs the slab, which needs us. This one
        /// costs nothing to stand up because a kernel ArchVmAspace just adopts
        /// the PGD kernel.phys already installed. Unused under the embedded
        /// layout, which maps nothing.
        ArchVmAspace m_aspace;

        ustl::Array<u32, MAX_CPU> m_cpu_to_unit;
        ustl::Array<u32, MAX_CPU> m_unit_to_group;
        ustl::Array<isize, MAX_CPU> m_offsets_per_unit;
        ustl::Array<u32, NUMA_MAX_NODE> m_group_to_node;

        u32 m_num_groups;
        usize m_populated;
        Layout m_layout;
    };

    /// Spreads the units over the nodes that have memory.
    ///
    /// This deliberately does not consult `cpu_to_node()`. The units are built
    /// before the MADT is parsed, so no CPU-to-node relation exists yet; node
    /// *memory* topology, on the other hand, is already known because
    /// PmNode::bootstrap() ran first. Even placement across those nodes is the
    /// best that can be done without CPU topology, and this is the one function
    /// that needs rewriting once CPU enumeration moves ahead of the PMM.
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

        for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
            m_cpu_to_unit[unit] = unit;
            m_unit_to_group[unit] = unit % m_num_groups;
        }
    }

    /// One buddy allocation per unit, taken from its group's node and used where
    /// the physmap already maps it. No address space is reserved and no page
    /// table is touched, which is the whole point: this is the layout for an
    /// architecture that has no window to spare.
    auto CpuLocal::Mapper::init_embedded() -> Status {
        auto const span = static_span() + ArchCpuLocal::kDynFirstChunkSize;
        auto const order = mm::get_order(align_up(span, PAGE_SIZE) / PAGE_SIZE);

        for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
            // kGafBoot implies Gaf::Zero, so a unit's dynamic area starts clean.
            auto frame = alloc_frames(node_of_unit(unit), mm::kGafBoot, order);
            if (!frame) {
                // Earlier units keep their blocks. The per-CPU area is never torn
                // down, and the only caller is init(), which panics.
                return Status::OutOfMem;
            }

            m_offsets_per_unit[unit] = isize(frame_to_virt(frame) - canonical_base());
        }

        m_populated = span;
        m_layout = Layout::Embedded;
        return Status::Ok;
    }

    /// Units at a constant stride through the reserved window, backed on demand.
    auto CpuLocal::Mapper::init_paged() -> Status {
        static_assert(usize(MAX_CPU) * kUnitStride <= usize(ARCH_CPU_LOCAL_SIZE),
                      "The reserved cpu local window cannot hold MAX_CPU units");
        static_assert(ArchCpuLocal::kUnitAlign <= kUnitStride,
                      "A unit must be at least as large as its alignment");

        auto status = m_aspace.init();
        if (Status::Ok != status) {
            return status;
        }

        for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
            m_offsets_per_unit[unit] = isize(paged_unit_base(unit) - canonical_base());
        }

        m_layout = Layout::Paged;
        m_populated = 0;

        // kGafBoot implies Gaf::Zero, so a unit's dynamic area starts clean.
        status = populate(static_span() + ArchCpuLocal::kDynFirstChunkSize, mm::kGafBoot);
        if (Status::Ok != status) {
            m_populated = 0;
        }
        return status;
    }

    auto CpuLocal::Mapper::populate(usize target, Gaf gaf) -> Status {
        DEBUG_ASSERT(Layout::Paged == m_layout, "The embedded layout cannot populate");

        target = align_up(target, PAGE_SIZE);
        if (target <= m_populated) {
            return Status::Ok;
        }
        if (target > kUnitStride) {
            return Status::OutOfRange;
        }

        // Cover the span with descending power-of-two blocks so it is met
        // exactly instead of overshooting to a single order.
        while (m_populated < target) {
            auto const remaining = (target - m_populated) / PAGE_SIZE;
            auto const order = ustl::algorithms::min(usize(ustl::bit_width(remaining)) - 1,
                                                     usize(MAX_FRAME_ORDER));
            auto const pages = usize(BIT(order));

            for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
                auto frame = alloc_frames(node_of_unit(unit), gaf, order);
                if (!frame) {
                    // Whatever was mapped for earlier units stays mapped. The
                    // per-CPU area is never torn down, and a failure here is
                    // either fatal (init) or simply refuses to grow.
                    return Status::OutOfMem;
                }

                auto mapped = usize(0);
                auto const status =
                    m_aspace.map(paged_unit_base(unit) + m_populated, frame_to_phys(frame), pages,
                                 kMapFlags, MapControl::ErrorIfExisting, &mapped);
                if (Status::Ok != status) {
                    return status;
                }
            }

            m_populated += pages * PAGE_SIZE;
        }

        return Status::Ok;
    }

    auto CpuLocal::Mapper::init() -> Status {
        distribute_units();

        // Prefer the paged layout where the architecture has a window for it, but
        // fall back rather than fail: the embedded layout needs strictly less
        // (no address space, no page tables) and still yields a working first
        // chunk -- one that simply cannot grow.
        if constexpr (ArchCpuLocal::kHasReservedWindow) {
            if (Status::Ok == init_paged()) {
                return Status::Ok;
            }
        }

        return init_embedded();
    }

    /// Runtime cpu local allocator.
    ///
    /// Hands out canonical addresses from the dynamic part of every unit. One
    /// reservation covers all CPUs at once -- the same canonical range is taken
    /// in every unit -- so a single bitmap describes them all and the allocator
    /// never needs to know how many CPUs there are.
    class CpuLocal::Dyn {
        using Self = CpuLocal::Dyn;

      public:
        auto init(Mapper &mapper) -> void;

        auto allocate(usize size, usize align) -> VirtAddr;

        auto deallocate(VirtAddr canonical, usize size) -> void;

        auto owns(VirtAddr canonical) const -> bool;

      private:
        /// How much address space one growth step claims. Sized so a chunk's
        /// bookkeeping still fits in the single page that carries it.
        constexpr static auto const kGrowStep = usize(KB(64));

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

        m_first_chunk.base = mapper.dyn_base();
        m_first_chunk.size = ArchCpuLocal::kDynFirstChunkSize;
        m_first_chunk.bitmap = ustl::views::make_span(m_first_bitmap.data(), kDynWords);
        m_first_chunk.num_cells = kDynCells;
        m_first_chunk.num_cells_used = 0;

        m_dynchunks.push_back(m_first_chunk);
    }

    /// Claims another slice of the reserved window and puts a chunk over it.
    ///
    /// The chunk header and its bitmap live in one buddy page rather than coming
    /// from the slab: CpuLocal must not depend on an allocator it is a
    /// prerequisite of, and that stays true after boot, not just during it.
    auto CpuLocal::Dyn::grow() -> bool {
        // Under the embedded layout there is nowhere to grow into: a second
        // block would sit at a different distance from the static copy on each
        // CPU, and no single offset could name it. The first chunk is all there
        // will ever be.
        if (!m_mapper->can_grow()) {
            return false;
        }

        auto const before = m_mapper->populated();
        auto const room = m_mapper->dyn_limit() - (canonical_base() + before);
        auto const step = ustl::algorithms::min(usize(kGrowStep), room);
        if (Status::Ok != m_mapper->populate(before + step, kGafGrow)) {
            return false;
        }
        auto const after = m_mapper->populated();

        auto frame = alloc_frames(kGafGrow, usize(0));
        if (!frame) {
            return false;
        }

        auto *page = reinterpret_cast<u8 *>(frame_to_virt(frame));
        auto const header = align_up(sizeof(DynChunk), alignof(usize));
        auto const words = (PAGE_SIZE - header) / sizeof(usize);

        auto *chunk = new (static_cast<void *>(page)) DynChunk();
        auto *bits = reinterpret_cast<usize *>(page + header);

        chunk->base = canonical_base() + before;
        chunk->size = after - before;
        chunk->num_cells = chunk->size / kDynCellSize;
        chunk->num_cells_used = 0;

        // A step whose bitmap would not fit in the page it shares is a
        // misconfiguration, not a runtime condition.
        DEBUG_ASSERT(chunk->num_cells <= words * kWordBits, "Growth step outruns its bitmap");
        chunk->bitmap = ustl::views::make_span(bits, words);

        m_dynchunks.push_back(*chunk);
        return true;
    }

    auto CpuLocal::Dyn::owns(VirtAddr canonical) const -> bool {
        Guard guard{m_lock};

        for (auto const &chunk : m_dynchunks) {
            if (canonical >= chunk.base && canonical < chunk.base + chunk.size) {
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

    auto CpuLocal::Dyn::deallocate(VirtAddr canonical, usize size) -> void {
        if (!canonical || !size) {
            return;
        }

        auto const cells = align_up(size, kDynCellSize) / kDynCellSize;

        Guard guard{m_lock};

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

    /// Zero rather than INVALID_CPU_NUM: until init() runs, the canonical copy
    /// *is* the boot CPU's storage, and the boot CPU is CPU 0. Cpu::id() reads
    /// this, and it has callers that run before init_early() installs anything.
    CPU_LOCAL
    decltype(CpuLocal::g_current_cpunum) CpuLocal::g_current_cpunum = 0;

    CPU_LOCAL
    decltype(CpuLocal::g_current_cpu_offset) CpuLocal::g_current_cpu_offset = 0;

    /// Static, because the slab cannot be used to build what the slab depends on.
    /// See the note above `CpuLocal::init()`.
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

        // The boot CPU is CPU 0 by construction, so there is nothing to look up.
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
    /// Everything it needs is therefore either static (the Mapper/Dyn maps, all
    /// bounded by MAX_CPU and NUMA_MAX_NODE) or comes straight from the buddy
    /// allocator (the frames behind the units, and the page tables that map
    /// them). That leaves exactly one precondition: PmNode::bootstrap() and
    /// EarlyMem::do_handoff() have run.
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
        // per-CPU state into the canonical area, and this is what carries it into
        // every unit rather than stranding it.
        for (auto cpu = CpuNum(0); cpu < MAX_CPU; ++cpu) {
            auto const offset = g_mapper->offset_of_unit(g_mapper->unit_of_cpu(cpu));
            auto *unit = reinterpret_cast<u8 *>(canonical_base() + offset);
            ustl::algorithms::copy_n(kInitialCpuLocalStart, initial_area_size(),
                                     reinterpret_cast<char *>(unit));
            g_cpu_offsets[cpu] = offset;

            // The copy carried the boot CPU's bookkeeping into every unit. Fix it
            // here, while `access()` still works from the outside, so a unit is
            // already self-describing before its CPU attaches to it.
            Self::access(g_current_cpu_offset, cpu) = offset;
            Self::access(g_current_cpunum, cpu) = cpu;
        }

        // The boot CPU has been running on the canonical area; move it across.
        auto const boot = current_cpu();
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
