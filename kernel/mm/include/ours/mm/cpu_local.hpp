/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_CPU_LOCAL_HPP
#define OURS_MM_CPU_LOCAL_HPP 1

#include <ours/arch/cpu_local.hpp>
#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/cpu_local_defs.hpp>
#include <ustl/function/invoke.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/util/move.hpp>
#include <ustl/util/nonconstructible.hpp>
#include <ustl/util/noncopyable.hpp>

#include <array>
#include <new>

namespace ours::mm {
/// Per-CPU storage.
///
/// The whole interface is one equation:
///
///     address = base(cpu) + offset
///
/// `offset` names an object. It is the same number on every CPU, it is what
/// callers hold, and it is never a pointer -- dereferencing one is meaningless.
/// `base(cpu)` is where that CPU's storage begins. The sum always lands inside
/// live storage: the static block, or one of the dynamic chunks. `holds()`
/// states that, and debug builds check it on every resolve.
///
/// A `CPU_LOCAL` variable is emitted once, by the linker. That definition is
/// never read or written -- it is a template to copy from, and `offset_of()`
/// measures against it. Each CPU owns a *unit*: its own copy of that block,
/// followed by room the dynamic allocator hands out.
///
/// Units are grouped by NUMA node, so their addresses are unrelated to each
/// other and `base()` is a lookup, not arithmetic on a stride. Dynamic chunks
/// are separate allocations that reproduce the same unit layout, which is what
/// lets one offset name an object in any of them:
///
///     base(cpu) ---> +-----------------+-------------------+
///                    | copy of the     | dynamic area      |
///                    | linker template |                   |
///                    +-----------------+-------------------+
///                     \__ offset 0 .. static_span __/
///
/// On architectures with a base register (%gs on x86) the register holds
/// `base(cpu) - origin()`, so a static object resolves in a single instruction:
/// the linker already baked `origin() + offset` into the instruction stream.
class CpuLocal {
    using Self = CpuLocal;

  public:
    template <typename T>
    class PerCpu;

    template <typename T, typename... Options>
    class Current;

    /// Offset value that names nothing. Zero is a real offset -- the first
    /// byte of the static block -- so emptiness needs its own value.
    constexpr static auto const NOWHERE = ~usize(0);

    /// Where CPU `cpu`'s storage begins.
    static auto base(CpuNum cpu) -> VirtAddr;

    /// Start of the address range offsets are measured from -- the linker
    /// template. Nothing is read or written through it.
    static auto origin() -> VirtAddr;

    /// The offset naming a statically declared `CPU_LOCAL` object.
    template <typename T>
    FORCE_INLINE static auto offset_of(T &object) -> usize {
        return reinterpret_cast<VirtAddr>(&object) - Self::origin();
    }

    /// Whether `offset` names storage that exists: inside the static block, or
    /// inside a live dynamic chunk. The contract every resolve relies on.
    [[nodiscard]]
    static auto holds(usize offset) -> bool;

    /// `offset` on the calling CPU. Uses the base register rather than a lookup.
    FORCE_INLINE static auto resolve(usize offset) -> VirtAddr {
        DEBUG_ASSERT(Self::holds(offset), "Resolving an offset that names nothing");
        return Self::origin() + offset + Self::read(g_current_cpu_offset);
    }

    /// `offset` on `cpu`.
    FORCE_INLINE static auto resolve(usize offset, CpuNum cpu) -> VirtAddr {
        DEBUG_ASSERT(Self::holds(offset), "Resolving an offset that names nothing");
        return Self::base(cpu) + offset;
    }

    /// Points the boot CPU at the linker-provided section so that `CPU_LOCAL`
    /// variables are usable before the physical allocator exists. Until `init()`
    /// runs the boot CPU *is* the canonical area, i.e. its offset is zero.
    static auto init_early() -> void;

    /// Gives every possible CPU its own unit and seeds it from the canonical
    /// area. Requires the physical allocator, so it runs at `InitStage::Pmm`.
    static auto init() -> void;

    /// Installs `cpu`'s unit on the CPU that calls it. Each secondary CPU runs
    /// this once, on itself, during bring-up.
    static auto init_percpu(CpuNum cpu) -> void;

    template <typename T>
    FORCE_INLINE static auto read(T &t) -> T {
        static_assert(sizeof(t) <= sizeof(usize));
        return ArchCpuLocal::read<T>(reinterpret_cast<isize>(&t));
    }

    template <typename T>
    FORCE_INLINE static auto write(T &t, T value) -> void {
        static_assert(sizeof(t) <= sizeof(usize));
        return ArchCpuLocal::write<T>(reinterpret_cast<isize>(&t), value);
    }

    template <typename T>
    FORCE_INLINE static auto access(T &t) -> T & {
        return *reinterpret_cast<T *>(reinterpret_cast<u8 *>(&t) +
                                      Self::read(g_current_cpu_offset));
    }

    template <typename T>
    FORCE_INLINE static auto access(T &t, CpuNum cpunum) -> T & {
        DEBUG_ASSERT(cpunum < MAX_CPU, "Invalid CPU Number");
        return *reinterpret_cast<T *>(reinterpret_cast<u8 *>(&t) + Self::g_cpu_offsets[cpunum]);
    }

    /// The CPU number the caller is running on, read out of its own unit.
    FORCE_INLINE static auto current_cpu() -> CpuNum {
        return Self::read(g_current_cpunum);
    }

    template <typename T, typename F>
    FORCE_INLINE static auto with_current(T &t, F &&f)
        -> ustl::traits::InvokeResultT<F, CpuNum, T &> {
        // current_cpu() rather than Cpu::id(): identical value, but Cpu::id()
        // is now a forwarder to it, and this is the hot path.
        auto const cpu = Self::current_cpu();
        DEBUG_ASSERT(Cpu::online_mask().test(cpu), "Invalid CPU Number");
        return ustl::function::invoke(f, cpu, Self::access(t, cpu));
    }

    /// For each instance on different CPU.
    template <typename T, typename F>
    FORCE_INLINE static auto for_each(T &t, F &&f) -> void {
        return Cpu::online_mask().for_each([&t, &f](CpuNum cpunum) {
            auto &current = Self::access(t, cpunum);
            ustl::function::invoke(f, cpunum, current);
        });
    }

    template <typename U, typename T, typename F>
    FORCE_INLINE static auto fold(T &t, U acc, F &&f) -> U {
        Cpu::online_mask().for_each([&acc, &t, &f](CpuNum cpunum) {
            auto &current = Self::access(t, cpunum);
            acc = ustl::function::invoke(f, cpunum, current, acc);
        });
        return acc;
    }

    /// Marks `cpus` as carrying a live unit. Called once the units built by
    /// `init()` have been handed to the CPUs that own them.
    static auto bootstrap(CpuMask const &cpus) -> void;

    /// Reserves `n` objects of `T` on every CPU slot and value-initializes each
    /// copy. The result is empty if the dynamic area is exhausted.
    ///
    /// Every slot up to MAX_CPU is constructed, not just the possible ones: the
    /// first chunk is built before the MADT has been parsed, so `possible_mask`
    /// is not yet meaningful when the slab calls this. Constructing the full
    /// range keeps a CPU that comes online later from finding raw storage.
    template <typename T>
    static auto allocate(usize n, usize align) -> PerCpu<T>;

    template <typename T>
    static auto free(PerCpu<T> percpu) -> void {
        percpu.reset();
    }

    /// Opaque here on purpose: both are defined in cpu-local.cpp, so an
    /// outside translation unit can never do anything with the names. They are
    /// public only because that file instantiates their storage at namespace
    /// scope ("static, because the slab cannot be used to build what the slab
    /// depends on"), which is not a member context and so cannot reach a
    /// private nested type.
    class Dyn;
    class Mapper;

  private:
    enum class State : u8 {
        Offline,
        Online,
        NumStates,
    };

    /// Binds the calling CPU to `cpunum`'s unit.
    static auto attach_to_unit(CpuNum cpunum, isize offset) -> void;

    /// Reserves `size` bytes on every CPU. Returns the offset naming them, or
    /// `NOWHERE` when the dynamic area is exhausted.
    static auto allocate_raw(usize size, usize align) -> usize;

    static auto free_raw(usize offset, usize size) -> void;

    static Mapper *g_mapper;
    static Dyn *g_dyn;

    static std::array<isize, MAX_CPU> g_cpu_offsets;
    static std::array<CpuMask, usize(State::NumStates)> g_states;
    static isize g_current_cpu_offset;
    static CpuNum g_current_cpunum;
};

/// Owning handle to a per-CPU allocation.
///
/// Holds an offset, never a pointer -- the same offset names the objects on
/// every CPU, and there is no single address that would be right for more than
/// one of them.
template <typename T>
class CpuLocal::PerCpu {
    using Self = PerCpu;

  public:
    PerCpu() : m_offset(NOWHERE), m_count(0) {}

    PerCpu(usize offset, usize count) : m_offset(offset), m_count(count) {}

    PerCpu(Self &&other) : m_offset(other.m_offset), m_count(other.m_count) {
        other.m_offset = NOWHERE;
        other.m_count = 0;
    }

    ~PerCpu() {
        reset();
    }

    auto operator=(Self &&other) -> Self & {
        if (this != &other) {
            reset();
            m_offset = other.m_offset;
            m_count = other.m_count;
            other.m_offset = NOWHERE;
            other.m_count = 0;
        }
        return *this;
    }

    /// The offset these objects are named by.
    [[nodiscard]]
    auto offset() const -> usize {
        return m_offset;
    }

    /// The calling CPU's copy.
    FORCE_INLINE auto ptr_mut() -> T * {
        return m_offset == NOWHERE ? nullptr : reinterpret_cast<T *>(CpuLocal::resolve(m_offset));
    }

    FORCE_INLINE auto ref_mut() -> T & {
        DEBUG_ASSERT(m_offset != NOWHERE, "Dereferencing an empty PerCpu");
        return *reinterpret_cast<T *>(CpuLocal::resolve(m_offset));
    }

    /// `cpunum`'s copy.
    FORCE_INLINE auto ref_mut(CpuNum cpunum) -> T & {
        DEBUG_ASSERT(m_offset != NOWHERE, "Dereferencing an empty PerCpu");
        return *reinterpret_cast<T *>(CpuLocal::resolve(m_offset, cpunum));
    }

    /// Unlike the static `CpuLocal::with_current`, the callback takes only the
    /// object: a handle already names one thing, and the CPU number is a
    /// `current_cpu()` away. This is the shape PmZone::alloc_frame() uses.
    template <typename F>
    FORCE_INLINE auto with_current(F &&f) -> ustl::traits::InvokeResultT<F, T &> {
        return ustl::function::invoke(f, ref_mut());
    }

    template <typename F>
    FORCE_INLINE auto for_each(F &&f) -> void {
        if (m_offset == NOWHERE) {
            return;
        }
        Cpu::online_mask().for_each(
            [this, &f](CpuNum cpunum) { ustl::function::invoke(f, cpunum, ref_mut(cpunum)); });
    }

    /// Destroys every CPU's copy and releases the reservation.
    auto reset() -> void {
        if (m_offset == NOWHERE) {
            return;
        }

        // Mirrors allocate(): every slot was constructed, so every slot is
        // destroyed, independent of which CPUs happen to be possible now.
        for (auto cpunum = CpuNum(0); cpunum < MAX_CPU; ++cpunum) {
            auto *base = reinterpret_cast<T *>(CpuLocal::resolve(m_offset, cpunum));
            for (auto i = m_count; i-- > 0;) {
                base[i].~T();
            }
        }

        CpuLocal::free_raw(m_offset, m_count * sizeof(T));
        m_offset = NOWHERE;
        m_count = 0;
    }

    [[nodiscard]]
    auto count() const -> usize {
        return m_count;
    }

    explicit operator bool() const {
        return m_offset != NOWHERE;
    }

  private:
    USTL_NOCOPYABLE(PerCpu);
    usize m_offset;
    usize m_count;
};

template <typename T>
auto CpuLocal::allocate(usize n, usize align) -> PerCpu<T> {
    auto const offset = Self::allocate_raw(n * sizeof(T), align);
    if (Self::NOWHERE == offset) {
        return PerCpu<T>();
    }

    for (auto cpunum = CpuNum(0); cpunum < MAX_CPU; ++cpunum) {
        auto *base = reinterpret_cast<T *>(Self::resolve(offset, cpunum));
        for (auto i = usize(0); i < n; ++i) {
            new (static_cast<void *>(base + i)) T();
        }
    }

    return PerCpu<T>(offset, n);
}

template <typename T, typename... Options>
class CpuLocal::Current {
  public:
    FORCE_INLINE static auto get() -> T & {
        return CpuLocal::access(g_current_instance);
    }

    FORCE_INLINE static auto get(CpuNum cpunum) -> T & {
        return CpuLocal::access(g_current_instance, cpunum);
    }

    FORCE_INLINE static auto set(T const &t) -> void {
        if constexpr (sizeof(t) <= sizeof(usize)) {
            CpuLocal::write(g_current_instance, t);
        } else {
            CpuLocal::access(g_current_instance) = t;
        }
    }

    FORCE_INLINE static auto set(T const &t, CpuNum cpunum) -> void {
        CpuLocal::access(g_current_instance, cpunum) = t;
    }

  protected:
    static T g_current_instance;
};

template <typename T, typename... Options>
CPU_LOCAL inline T CpuLocal::Current<T, Options...>::g_current_instance;

} // namespace ours::mm

#endif // #ifndef OURS_MM_CPU_LOCAL_HPP
