#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/arch_vm_aspace.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vmas_flags.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ustl/bit.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/views/span.hpp>

/// An architecture whose ArchCpuLocal reports `HAS_RESERVED_WINDOW == false` has
/// no window to describe, and should not have to invent one to compile this
/// file. The paged chunk allocator is never selected there, so these are never
/// read.
#ifndef ARCH_CPU_LOCAL_VIRT_BASE
#include <algorithm>
#include <array>
#define ARCH_CPU_LOCAL_VIRT_BASE 0
#define ARCH_CPU_LOCAL_CHUNK_SLOT PAGE_SIZE
#define ARCH_CPU_LOCAL_SIZE 0
#endif

using ustl::mm::align_up;

extern char const INITIAL_CPU_LOCAL_START[] LINK_NAME("__cpu_local_start");
extern char const INITIAL_CPU_LOCAL_END[] LINK_NAME("__cpu_local_end");

namespace ours::mm {
namespace {
    /// Allocation granularity of the dynamic area. Every reservation is a whole
    /// number of cells so the bitmap has one bit per cell.
    constexpr auto DYN_CELL_SIZE = usize(sizeof(usize));

    constexpr auto WORD_BITS = usize(sizeof(usize) * 8);

    /// Chunk 0's dynamic area is exactly `DYN_FIRST_CHUNK_SIZE` per CPU (see
    /// Mapper::unit_size), so its bitmap is a compile-time size and needs no
    /// allocator at all.
    constexpr auto DYN_CELLS = ArchCpuLocal::DYN_FIRST_CHUNK_SIZE / DYN_CELL_SIZE;
    constexpr auto DYN_WORDS = (DYN_CELLS + WORD_BITS - 1) / WORD_BITS;

    constexpr auto MAP_FLAGS = MmuFlags::Present | MmuFlags::Readable | MmuFlags::Writable;

    /// Growth runs under Dyn's spin lock, i.e. with preemption off, so it must
    /// not take a path that can reclaim or wait. GAF_BOOT and GAF_KERNEL both
    /// carry DirectReclaim; this one refuses instead, and a refusal only means
    /// the allocation fails.
    constexpr auto GAF_GROW = Gaf::ThisNode | Gaf::Zero | Gaf::NoRetry;

    FORCE_INLINE auto initial_area_size() -> usize {
        return INITIAL_CPU_LOCAL_END - INITIAL_CPU_LOCAL_START;
    }

    /// The address the linker gave the template, and so the base of the range
    /// every handle lives in.
    ///
    /// Nothing is ever read or written through it after init_early(). It is the
    /// zero point that `access()` measures from, no more -- which matters,
    /// because the template itself lives in the `.init` area and is handed back
    /// to the allocator by reclaim_init_area().
    FORCE_INLINE auto canonical_base() -> VirtAddr {
        return VirtAddr(INITIAL_CPU_LOCAL_START);
    }

    FORCE_INLINE auto is_in_initial_area(VirtAddr va) -> bool {
        return va >= VirtAddr(INITIAL_CPU_LOCAL_START) && va < VirtAddr(INITIAL_CPU_LOCAL_END);
    }

    /// Bytes the static half of a unit occupies, rounded so the dynamic area
    /// behind it starts on a page boundary.
    FORCE_INLINE auto static_span() -> usize {
        return align_up(initial_area_size(), ArchCpuLocal::UNIT_ALIGN);
    }
} // namespace

struct DynChunk: ustl::collections::intrusive::ListBaseHook<> {
    /// Handle naming the first byte this chunk manages.
    VirtAddr base;
    usize size;

    /// One bit per `DYN_CELL_SIZE` bytes; set means taken.
    ustl::views::Span<usize> bitmap;

    usize num_cells;
    usize num_cells_used;
};

/// Places the per-CPU units.
///
/// A *chunk* is one reservation covering every CPU at once, divided into
/// `MAX_CPU` units of `unit_size` each:
///
///     chunk_base
///     |
///     +----------+----------+-----+----------+
///     |  unit 0  |  unit 1  | ... |  unit N  |
///     +----------+----------+-----+----------+
///      \_unit_size_/
///
/// Every chunk repeats that spacing, which is the whole trick: an object at
/// `B + n * unit_size + d` is named by `handle = origin + (B - m_base) + d`,
/// with the per-CPU term cancelled. `handle + offset(cpu)` is then correct on
/// every CPU, in any chunk, without the caller or the allocator knowing which
/// chunk it fell in.
///
/// Reproducing the spacing requires being able to *choose* where a chunk's
/// units land, so the units are packed densely in **address space** while
/// each one's frames still come from its owner's node. Locality is paid for
/// by the page tables, not by scattering the layout.
///
/// `Scattered` is the fallback for an architecture with no window to spare:
/// one buddy allocation per node, units contiguous only within a node. The
/// resulting table cannot be reproduced by a later chunk -- the span from
/// the lowest allocation to the highest is unbounded -- so that layout gets
/// the first chunk and no growth, and `can_grow()` says so rather than
/// failing silently later.
class CpuLocal::Mapper {
    using Self = CpuLocal::Mapper;

  public:
    enum class Layout : u8 {
        Dense,
        Scattered,
    };

    Mapper()
        : m_aspace(ARCH_CPU_LOCAL_VIRT_BASE, ARCH_CPU_LOCAL_SIZE, VmAspaceKind::Kernel)
        , m_c2u()
        , m_u2c()
        , m_n2g()
        , m_g2n()
        , m_unit_group()
        , m_unit_offset()
        , m_group_num_units()
        , m_group_frames()
        , m_base(0)
        , m_replica(0)
        , m_num_units(0)
        , m_num_groups(0)
        , m_next_slot(0)
        , m_layout(Layout::Scattered) {}

    /// Bytes one CPU owns in a chunk, and the spacing every chunk repeats.
    ///
    /// `static_span()` is a multiple of `UNIT_ALIGN` and `DYN_FIRST_CHUNK_SIZE`
    /// is a multiple of a page, so this is exactly
    /// `static_span() + DYN_FIRST_CHUNK_SIZE` -- which is what lets chunk 0's
    /// dynamic tail be a compile-time size.
    static auto unit_size() -> usize {
        return align_up(static_span() + ArchCpuLocal::DYN_FIRST_CHUNK_SIZE,
                        ArchCpuLocal::UNIT_ALIGN);
    }

    /// What one chunk spans: every unit, back to back.
    static auto chunk_span() -> usize {
        return unit_size() * MAX_CPU;
    }

    /// The provisional unit, rounded to a whole buddy order.
    ///
    /// Only this allocation is rounded, and only so that init() can hand it
    /// back with a single `free_frames`. Rounding `unit_size()` itself would
    /// inflate every unit of every chunk to buy that one convenience.
    static auto replica_span() -> usize {
        auto const pages = align_up(unit_size(), PAGE_SIZE) / PAGE_SIZE;
        return usize(BIT(mm::get_order(pages))) * PAGE_SIZE;
    }

    /// Claims the boot CPU's provisional unit from the boot allocator.
    ///
    /// One unit, for CPU 0, and nothing else -- no other CPU is enumerated
    /// this early, and there is no PMM to map anything with yet.
    auto init_early() -> Status;

    /// Builds chunk 0 and the unit table.
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

    /// `CPU_OFFSETS[cpu]` for a unit -- what `access()` adds.
    auto offset_of_unit(u32 unit) const -> isize {
        return isize(unit_base(unit) - canonical_base());
    }

    /// The handle naming an address in CPU 0's slice of some chunk.
    auto handle_of_address(VirtAddr real) const -> VirtAddr {
        return canonical_base() + (real - m_base);
    }

    /// Offset at which every unit's dynamic area begins.
    static auto dyn_offset() -> usize {
        return static_span();
    }

    /// Takes another chunk, laid out like the first. Returns its base, or 0.
    auto alloc_chunk(Gaf gaf) -> VirtAddr;

    auto can_grow() const -> bool {
        return Layout::Dense == m_layout && m_next_slot < NUM_SLOTS;
    }

  private:
    constexpr static auto NUM_SLOTS =
        usize(ARCH_CPU_LOCAL_SIZE) / usize(ARCH_CPU_LOCAL_CHUNK_SLOT);

    auto build_map_info() -> void;

    auto build_dense(VirtAddr slot, Gaf gaf) -> Status;

    auto build_scattered() -> Status;

    auto slot_at(usize index) const -> VirtAddr {
        return VirtAddr(ARCH_CPU_LOCAL_VIRT_BASE) + index * usize(ARCH_CPU_LOCAL_CHUNK_SLOT);
    }

    auto node_of_unit(u32 unit) const -> NnId {
        return NnId(m_g2n[m_unit_group[unit]]);
    }

    /// Private rather than borrowed from VmAspace::kernel_aspace(): that one
    /// is built by init_vmm(), which needs the slab, which needs us. This one
    /// costs nothing to stand up because a kernel ArchVmAspace just adopts
    /// the PGD kernel.phys already installed.
    ArchVmAspace m_aspace;

    std::array<i32, MAX_CPU> m_c2u;        // CPU   -> unit
    std::array<i32, MAX_CPU> m_u2c;        // unit  -> CPU
    std::array<i32, NUMA_MAX_NODE> m_n2g;  // node  -> group
    std::array<i32, NUMA_MAX_NODE> m_g2n;  // group -> node
    std::array<u32, MAX_CPU> m_unit_group; // unit  -> group
    std::array<isize, MAX_CPU> m_unit_offset;
    std::array<u32, NUMA_MAX_NODE> m_group_num_units;
    std::array<PmFrame::List, NUMA_MAX_NODE> m_group_frames;

    VirtAddr m_base;
    VirtAddr m_replica;
    u32 m_num_units;
    u32 m_num_groups;
    usize m_next_slot;
    Layout m_layout;
};

auto CpuLocal::Mapper::init_early() -> Status {
    auto const span = replica_span();
    auto *block = EarlyMem::allocate<u8>(span, span, bootmem::ANY_NODE);
    if (!block) {
        return Status::OutOfMem;
    }

    m_replica = VirtAddr(block);
    return Status::Ok;
}

/// Builds the node groups and assigns every CPU slot a unit.
///
/// A unit is created for all `MAX_CPU` slots, not just the CPUs that are
/// currently possible: chunk 0 is built before the MADT is parsed, so
/// `possible_mask()` is not yet meaningful, and `allocate()` constructs on
/// every slot. Leaving a slot without a unit would leave its offset at zero,
/// which resolves into the template -- the one region that must never be
/// written.
///
/// Groups exist to source frames per node. `node_hard_affinity()` is
/// consulted where it answers, and the rest are spread round-robin over the
/// nodes that have memory; real affinity has to wait for CPU enumeration to
/// move ahead of the PMM.
auto CpuLocal::Mapper::build_map_info() -> void {
    auto const &nodes = mm::node_possible_mask();

    m_num_groups = 0;
    for (auto nid = usize(0); nid < nodes.size() && m_num_groups < NUMA_MAX_NODE; ++nid) {
        if (nodes.test(nid)) {
            m_n2g[nid] = i32(m_num_groups);
            m_g2n[m_num_groups] = i32(nid);
            ++m_num_groups;
        }
    }
    if (!m_num_groups) {
        m_g2n[0] = 0;
        m_num_groups = 1;
    }

    for (auto &count: m_group_num_units) {
        count = 0;
    }

    for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
        auto const cpu = CpuNum(unit);
        m_c2u[cpu] = i32(unit);
        m_u2c[unit] = i32(cpu);

        auto group = u32(unit % m_num_groups);
        for (auto g = u32(0); g < m_num_groups; ++g) {
            auto const *affinity = mm::node_hard_affinity(NnId(m_g2n[g]));
            if (affinity && affinity->test(cpu)) {
                group = g;
                break;
            }
        }

        m_unit_group[unit] = u32(group);
        ++m_group_num_units[group];
    }

    m_num_units = MAX_CPU;
}

/// Units packed back to back in the reserved window, each backed from its
/// own node.
///
/// The packing is what makes the table reproducible; the per-unit node is
/// what keeps allocation local. Both at once is only possible because the
/// address and the frames are chosen independently.
auto CpuLocal::Mapper::build_dense(VirtAddr slot, Gaf gaf) -> Status {
    auto const pages = align_up(unit_size(), PAGE_SIZE) / PAGE_SIZE;

    for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
        auto const va = slot + usize(unit) * unit_size();
        auto const nid = node_of_unit(unit);

        // Exactly `pages`, walked with descending power-of-two blocks:
        // rounding up to one order would map past the unit and collide with
        // the next, since unit_size need not be a power of two pages.
        for (auto done = usize(0); done < pages;) {
            auto const remaining = pages - done;
            auto const order =
                std::min(usize(ustl::bit_width(remaining)) - 1, usize(MAX_FRAME_ORDER));
            auto const n = usize(BIT(order));

            auto frame = alloc_frames(nid, gaf, order);
            if (!frame) {
                // Earlier units stay mapped. Nothing here is ever torn down,
                // and the only caller that can fail is init(), which panics.
                return Status::OutOfMem;
            }

            auto mapped = usize(0);
            auto const status = m_aspace.map(va + done * PAGE_SIZE, frame_to_phys(frame), n,
                                             MAP_FLAGS, MapControl::ErrorIfExisting, &mapped);
            if (Status::Ok != status) {
                return status;
            }

            done += n;
        }
    }

    return Status::Ok;
}

/// One buddy allocation per node, units contiguous only within a node.
///
/// Costs no address space and touches no page table, which is the point --
/// but the table it produces spans from the lowest allocation to the
/// highest, so no later chunk can reproduce it. This layout gets chunk 0
/// and nothing more.
auto CpuLocal::Mapper::build_scattered() -> Status {
    auto const size = unit_size();

    m_base = ustl::NumericLimits<VirtAddr>::max();
    for (auto group = u32(0); group < m_num_groups; ++group) {
        auto const nr_units = m_group_num_units[group];
        if (!nr_units) {
            continue;
        }

        auto const nid = NnId(m_g2n[group]);
        auto const order = mm::get_order(align_up(size * nr_units, PAGE_SIZE) / PAGE_SIZE);
        if (order > MAX_FRAME_ORDER) {
            return Status::OutOfRange;
        }

        // GAF_BOOT implies Gaf::Zero, so a unit's dynamic area starts clean.
        auto frame = alloc_frames(nid, mm::GAF_BOOT, order);
        if (!frame) {
            for (auto done = u32(0); done < group; ++done) {
                free_frames(&m_group_frames[done]);
            }
            return Status::OutOfMem;
        }

        m_group_frames[group].push_back(*frame);
        m_base = std::min(m_base, frame_to_virt(frame));
    }

    for (auto group = u32(0); group < m_num_groups; ++group) {
        if (!m_group_num_units[group]) {
            continue;
        }

        auto const addr = frame_to_virt(&m_group_frames[group].front().value());
        auto slot = u32(0);
        for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
            if (m_unit_group[unit] == group) {
                m_unit_offset[unit] = isize(addr + size * slot - m_base);
                ++slot;
            }
        }
    }

    return Status::Ok;
}

auto CpuLocal::Mapper::init() -> Status {
    static_assert(usize(ARCH_CPU_LOCAL_SIZE) % usize(ARCH_CPU_LOCAL_CHUNK_SLOT) == 0,
                  "The cpu local window must divide into whole slots");

    build_map_info();
    if (!m_num_units) {
        return Status::InvalidArguments;
    }

    if constexpr (ArchCpuLocal::HAS_RESERVED_WINDOW) {
        if (chunk_span() <= usize(ARCH_CPU_LOCAL_CHUNK_SLOT) && Status::Ok == m_aspace.init()) {
            auto const slot = slot_at(m_next_slot);
            // GAF_BOOT implies Gaf::Zero, so a unit's dynamic area starts
            // clean.
            if (Status::Ok == build_dense(slot, mm::GAF_BOOT)) {
                ++m_next_slot;
                m_base = slot;
                for (auto unit = u32(0); unit < MAX_CPU; ++unit) {
                    m_unit_offset[unit] = isize(usize(unit) * unit_size());
                }
                m_layout = Layout::Dense;
                return Status::Ok;
            }
        }
    }

    m_layout = Layout::Scattered;
    return build_scattered();
}

auto CpuLocal::Mapper::alloc_chunk(Gaf gaf) -> VirtAddr {
    if (!can_grow()) {
        return 0;
    }

    auto const slot = slot_at(m_next_slot);
    if (Status::Ok != build_dense(slot, gaf)) {
        return 0;
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
        return (chunk.bitmap[cell / WORD_BITS] & (usize(1) << (cell % WORD_BITS))) != 0;
    }

    static auto set(DynChunk &chunk, usize cell, bool value) -> void {
        auto const mask = usize(1) << (cell % WORD_BITS);
        if (value) {
            chunk.bitmap[cell / WORD_BITS] |= mask;
        } else {
            chunk.bitmap[cell / WORD_BITS] &= ~mask;
        }
    }

    mutable sync::SpinLock m_lock;
    Mapper *m_mapper;
    ustl::collections::intrusive::List<DynChunk> m_dynchunks;
    DynChunk m_first_chunk;
    std::array<usize, DYN_WORDS> m_first_bitmap;
};

auto CpuLocal::Dyn::init(Mapper &mapper) -> void {
    m_mapper = &mapper;

    for (auto &word: m_first_bitmap) {
        word = 0;
    }

    // Chunk 0 is the one whose unit carries the static copy, so its dynamic
    // part is what is left over -- exactly DYN_FIRST_CHUNK_SIZE, since
    // static_span() is already a multiple of UNIT_ALIGN.
    m_first_chunk.base = canonical_base() + Mapper::dyn_offset();
    m_first_chunk.size = ArchCpuLocal::DYN_FIRST_CHUNK_SIZE;
    m_first_chunk.bitmap = ustl::views::make_span(m_first_bitmap.data(), DYN_WORDS);
    m_first_chunk.num_cells = DYN_CELLS;
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

    auto const base = m_mapper->alloc_chunk(GAF_GROW);
    if (!base) {
        return false;
    }

    auto frame = alloc_frames(GAF_GROW, usize(0));
    if (!frame) {
        return false;
    }

    auto *page = reinterpret_cast<u8 *>(frame_to_virt(frame));
    auto const header = align_up(sizeof(DynChunk), alignof(usize));
    auto const words = (PAGE_SIZE - header) / sizeof(usize);

    auto *chunk = new (static_cast<void *>(page)) DynChunk();
    auto *bits = reinterpret_cast<usize *>(page + header);

    chunk->base = m_mapper->handle_of_address(base);
    chunk->size = Mapper::unit_size();
    chunk->num_cells = chunk->size / DYN_CELL_SIZE;
    chunk->num_cells_used = 0;

    // A unit whose bitmap would not fit in the page it shares is a
    // misconfiguration, not a runtime condition.
    DEBUG_ASSERT(chunk->num_cells <= words * WORD_BITS, "Unit size outruns its bitmap");
    chunk->bitmap = ustl::views::make_span(bits, words);

    m_dynchunks.push_back(*chunk);
    return true;
}

auto CpuLocal::Dyn::owns(VirtAddr handle) const -> bool {
    Guard guard{m_lock};

    for (auto const &chunk: m_dynchunks) {
        if (handle >= chunk.base && handle < chunk.base + chunk.size) {
            return true;
        }
    }
    return false;
}

auto CpuLocal::Dyn::allocate_locked(usize cells, usize align_bytes) -> VirtAddr {
    auto const step = align_bytes / DYN_CELL_SIZE;

    for (auto &chunk: m_dynchunks) {
        if (chunk.num_cells_used + cells > chunk.num_cells) {
            continue;
        }

        // Alignment is a property of the address, not of the cell index, so
        // anchor the scan on the first cell whose address qualifies and step
        // in whole multiples of the alignment from there.
        auto const first = (align_up(chunk.base, align_bytes) - chunk.base) / DYN_CELL_SIZE;

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
            return chunk.base + start * DYN_CELL_SIZE;
        }
    }

    return NOWHERE;
}

auto CpuLocal::Dyn::allocate(usize size, usize align) -> VirtAddr {
    if (!size) {
        return NOWHERE;
    }

    auto const cells = align_up(size, DYN_CELL_SIZE) / DYN_CELL_SIZE;
    auto const align_bytes = std::max(align, DYN_CELL_SIZE);
    DEBUG_ASSERT(!(align_bytes & (align_bytes - 1)), "Alignment must be a power of two");
    DEBUG_ASSERT(align_bytes <= ArchCpuLocal::UNIT_ALIGN,
                 "Alignment beyond a unit boundary is not supported");

    Guard guard{m_lock};

    auto found = allocate_locked(cells, align_bytes);
    if (NOWHERE == found && grow()) {
        found = allocate_locked(cells, align_bytes);
    }
    return found;
}

auto CpuLocal::Dyn::deallocate(VirtAddr handle, usize size) -> void {
    if (handle == NOWHERE || !size) {
        return;
    }

    auto const cells = align_up(size, DYN_CELL_SIZE) / DYN_CELL_SIZE;

    Guard guard{m_lock};

    for (auto &chunk: m_dynchunks) {
        if (handle < chunk.base || handle >= chunk.base + chunk.size) {
            continue;
        }

        auto const start = (handle - chunk.base) / DYN_CELL_SIZE;
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

decltype(CpuLocal::STATES) CpuLocal::STATES;
decltype(CpuLocal::CPU_OFFSETS) CpuLocal::CPU_OFFSETS;

/// Zero rather than INVALID_CPU_NUM: the template is what CPU 0 reads until
/// init_early() moves it onto a real unit, and the boot CPU is CPU 0.
CPU_LOCAL
decltype(CpuLocal::CURRENT_CPUNUM) CpuLocal::CURRENT_CPUNUM = 0;

CPU_LOCAL
decltype(CpuLocal::CURRENT_CPU_OFFSET) CpuLocal::CURRENT_CPU_OFFSET = 0;

/// Static, because the slab cannot be used to build what the slab depends on.
namespace {
    CpuLocal::Mapper MAPPER_STORAGE;
    CpuLocal::Dyn DYN_STORAGE;
} // namespace

CpuLocal::Mapper *CpuLocal::MAPPER;
CpuLocal::Dyn *CpuLocal::DYN;

/// Order matters: the base register has to be live before any `read`/`write`
/// can reach the unit, so it is installed first and the bookkeeping written
/// through it afterwards. Writing first would land in the template, which is
/// exactly what must not be written.
auto CpuLocal::attach_to_unit(CpuNum cpunum, isize offset) -> void {
    // %gs holds `base(cpu) - origin()`, so every %gs displacement -- in C++
    // and in assembly -- is the object's own address, and the segment base
    // supplies the rest. This is what Linux loads into MSR_GS_BASE; see
    // cpu_kernelmode_gs_base() in its arch/x86/include/asm/processor.h.
    ArchCpuLocal::install(offset);
    Self::write(CURRENT_CPU_OFFSET, offset);
    Self::write(CURRENT_CPUNUM, cpunum);
}

/// Gets the boot CPU onto a unit of its own, as early as possible.
///
/// It cannot simply run on the template itself. That memory belongs
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
    MAPPER = &MAPPER_STORAGE;
    ASSERT(Status::Ok == MAPPER->init_early(), "Failed to claim the boot cpu local unit");

    for (auto &offset: CPU_OFFSETS) {
        offset = 0;
    }

    // The boot CPU is CPU 0 by construction, so there is nothing to look up
    // -- which also avoids asking Cpu::id(), whose answer lives in the very
    // storage being set up here.
    auto const boot = CpuNum(0);
    auto const replica = MAPPER->replica();
    auto const offset = isize(replica - canonical_base());

    std::copy_n(INITIAL_CPU_LOCAL_START, initial_area_size(), reinterpret_cast<char *>(replica));

    CPU_OFFSETS[boot] = offset;
    attach_to_unit(boot, offset);

    // The first dynamic chunk lives in the tail of this unit. It is recorded
    // as a handle, so it survives the migration in init() untouched -- the
    // same handle simply resolves against a different unit base.
    DYN = &DYN_STORAGE;
    DYN->init(*MAPPER);

    STATES[usize(State::Online)].reset().set(boot);
    STATES[usize(State::Offline)].reset();
}

/// Builds the real per-CPU areas, once the PMM exists.
///
/// This has to run *before* the slab is initialised, and must not allocate
/// through it, because the slab is one of its users: MemCache::init() ->
/// CpuCache::on_mem_cache_init() -> CpuLocal::allocate<CpuCache>(). Taking
/// even one kmalloc() here would close the loop.
auto CpuLocal::init() -> void {
    DEBUG_ASSERT(MAPPER && MAPPER->replica(), "CpuLocal::init_early() has not run");

    ASSERT(Status::Ok == MAPPER->init(), "Failed to lay out cpu local units");

    auto const replica = MAPPER->replica();
    auto const nr_units = MAPPER->num_units();

    for (auto unit = u32(0); unit < nr_units; ++unit) {
        auto const cpu = MAPPER->unit_to_cpu(unit);
        auto const offset = MAPPER->offset_of_unit(unit);
        auto *base = reinterpret_cast<u8 *>(MAPPER->unit_base(unit));

        if (cpu == 0) {
            // CPU 0 has been running on the provisional unit since
            // init_early(); carry what it accumulated across, dynamic tail
            // included, rather than resetting it to the defaults.
            std::copy_n(reinterpret_cast<u8 const *>(replica), Mapper::unit_size(), base);
        } else {
            // Everyone else starts from the template. Copying CPU 0's unit
            // here instead would hand every CPU CPU 0's PmNode::Current and
            // apic_id.
            std::copy_n(INITIAL_CPU_LOCAL_START, initial_area_size(),
                        reinterpret_cast<char *>(base));
        }

        CPU_OFFSETS[cpu] = offset;
    }

    // Only now that every offset is published can access() reach a unit from
    // the outside; fix each one's own bookkeeping so it is self-describing
    // before its CPU attaches.
    for (auto unit = u32(0); unit < nr_units; ++unit) {
        auto const cpu = MAPPER->unit_to_cpu(unit);
        Self::access(CURRENT_CPU_OFFSET, cpu) = CPU_OFFSETS[cpu];
        Self::access(CURRENT_CPUNUM, cpu) = cpu;
    }

    // Move CPU 0 onto its real unit and release the provisional one. Only
    // that allocation was rounded to a whole buddy order, so this hands back
    // exactly what was taken without inflating unit_size() everywhere.
    attach_to_unit(0, CPU_OFFSETS[0]);
    free_frames(replica, mm::get_order(Mapper::replica_span() / PAGE_SIZE));

    bootstrap(CpuMask::of(0));
}

auto CpuLocal::init_percpu(CpuNum cpu) -> void {
    DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
    DEBUG_ASSERT(DYN, "Bringing up a CPU before CpuLocal::init()");

    attach_to_unit(cpu, CPU_OFFSETS[cpu]);

    STATES[usize(State::Online)].set(cpu);
    STATES[usize(State::Offline)].set(cpu, false);
}

auto CpuLocal::bootstrap(CpuMask const &cpus) -> void {
    cpus.for_each([](CpuNum cpu) {
        STATES[usize(State::Online)].set(cpu);
        STATES[usize(State::Offline)].set(cpu, false);
    });
}

auto CpuLocal::origin() -> VirtAddr {
    return canonical_base();
}

auto CpuLocal::base(CpuNum cpu) -> VirtAddr {
    DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
    return canonical_base() + usize(CPU_OFFSETS[cpu]);
}

/// The contract `resolve()` rests on: a handle names either a byte of the
/// static block or a byte of some live dynamic chunk, and nothing else.
auto CpuLocal::holds(VirtAddr handle) -> bool {
    if (is_in_initial_area(handle)) {
        return true;
    }
    return DYN && DYN->owns(handle);
}

auto CpuLocal::allocate_raw(usize size, usize align) -> VirtAddr {
    if (!DYN) {
        return NOWHERE;
    }
    return DYN->allocate(size, align);
}

auto CpuLocal::free_raw(VirtAddr handle, usize size) -> void {
    if (!DYN) {
        return;
    }
    return DYN->deallocate(handle, size);
}

} // namespace ours::mm
