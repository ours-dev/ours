/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_CPU_LOCAL_HPP
#define OURS_MM_CPU_LOCAL_HPP 1

#include <ours/arch/cpu-local.hpp>
#include <ours/assert.hpp>
#include <ours/cpu-mask.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu-local-defs.hpp>

#include <new>

#include <ustl/array.hpp>
#include <ustl/function/invoke.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/util/move.hpp>
#include <ustl/util/nonconstructible.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::mm {
/// Per-CPU storage.
///
/// A `CPU_LOCAL` variable is emitted once, into the `.init.data.cpu_local`
/// section delimited by `__cpu_local_start` / `__cpu_local_end`. That single
/// definition is the *canonical* address of the variable; it is never accessed
/// directly once `init()` has run.
///
/// Each CPU owns a `unit`: a private copy of the whole section, followed by a
/// dynamic area that `allocate()` carves up. For CPU `n` the copy of a
/// canonical address `A` lives at `A + g_cpu_offsets[n]`, so a single offset
/// per CPU relocates every per-CPU variable at once. `g_cpu_offsets[n]` is also
/// what the architecture installs as its per-CPU base register (%gs on x86),
/// which is what lets `read()` / `write()` reach the current CPU's copy in one
/// instruction, without loading the CPU number first.
///
/// Layout of one unit:
///
///     unit_base(n)                                        unit_base(n) + unit_size
///     |                                                                          |
///     +--------------------------------+-----------------------------------------+
///     | copy of [start, end)           | dynamic area                            |
///     +--------------------------------+-----------------------------------------+
///      \__ static, linker-allocated __/ \__ handed out by allocate() __/
///
class CpuLocal {
    using Self = CpuLocal;

  public:
    template <typename T>
    class PerCpu;

    template <typename T, typename... Options>
    class Current;

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
    FORCE_INLINE
    static auto current_cpu() -> CpuNum {
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

    /// Translate a cpu local address to according virtual address.
    ///
    /// Returns 0 if `va` does not name per-CPU storage.
    static auto translate(VirtAddr va) -> VirtAddr;

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

  private:
    enum class State : u8 {
        Offline,
        Online,
        NumStates,
    };

    class Dyn;
    class Mapper;

    /// Binds the calling CPU to `cpunum`'s unit.
    static auto attach_to_unit(CpuNum cpunum, isize offset) -> void;

    /// Reserves `size` bytes on every CPU. Returns a canonical address, i.e. one
    /// that `access()` and `translate()` can relocate, or 0 on failure.
    static auto allocate_raw(usize size, usize align) -> VirtAddr;

    static auto free_raw(VirtAddr canonical, usize size) -> void;

    static Mapper *g_mapper;
    static Dyn *g_dyn;

    static ustl::Array<isize, MAX_CPU> g_cpu_offsets;
    static ustl::Array<CpuMask, usize(State::NumStates)> g_states;
    static isize g_current_cpu_offset;
    static CpuNum g_current_cpunum;
};

/// Owning handle to a per-CPU allocation.
///
/// Holds a canonical address, so the same handle names the object on every CPU;
/// `ref_mut()` resolves it against whichever CPU asks.
template <typename T>
class CpuLocal::PerCpu {
    using Self = PerCpu;

  public:
    PerCpu() : m_pointer(nullptr), m_count(0) {}

    PerCpu(T *pointer, usize count) : m_pointer(pointer), m_count(count) {}

    PerCpu(Self &&other) : m_pointer(other.m_pointer), m_count(other.m_count) {
        other.m_pointer = nullptr;
        other.m_count = 0;
    }

    ~PerCpu() {
        reset();
    }

    auto operator=(Self &&other) -> Self & {
        if (this != &other) {
            reset();
            m_pointer = other.m_pointer;
            m_count = other.m_count;
            other.m_pointer = nullptr;
            other.m_count = 0;
        }
        return *this;
    }

    /// The calling CPU's copy.
    FORCE_INLINE
    auto ptr_mut() -> T * {
        return m_pointer ? &CpuLocal::access(*m_pointer) : nullptr;
    }

    FORCE_INLINE
    auto ref_mut() -> T & {
        DEBUG_ASSERT(m_pointer, "Dereferencing an empty PerCpu");
        return CpuLocal::access(*m_pointer);
    }

    /// `cpunum`'s copy.
    FORCE_INLINE
    auto ref_mut(CpuNum cpunum) -> T & {
        DEBUG_ASSERT(m_pointer, "Dereferencing an empty PerCpu");
        return CpuLocal::access(*m_pointer, cpunum);
    }

    /// Unlike the static `CpuLocal::with_current`, the callback takes only the
    /// object: a handle already names one thing, and the CPU number is a
    /// `current_cpu()` away. This is the shape PmZone::alloc_frame() uses.
    template <typename F>
    FORCE_INLINE auto with_current(F &&f) -> ustl::traits::InvokeResultT<F, T &> {
        DEBUG_ASSERT(m_pointer, "Dereferencing an empty PerCpu");
        return ustl::function::invoke(f, CpuLocal::access(*m_pointer));
    }

    template <typename F>
    FORCE_INLINE auto for_each(F &&f) -> void {
        if (m_pointer) {
            CpuLocal::for_each(*m_pointer, ustl::forward<F>(f));
        }
    }

    /// Destroys every CPU's copy and releases the reservation.
    auto reset() -> void {
        if (!m_pointer) {
            return;
        }

        // Mirrors allocate(): every slot was constructed, so every slot is
        // destroyed, independent of which CPUs happen to be possible now.
        for (auto cpunum = CpuNum(0); cpunum < MAX_CPU; ++cpunum) {
            auto *base = &CpuLocal::access(*m_pointer, cpunum);
            for (auto i = m_count; i-- > 0;) {
                base[i].~T();
            }
        }

        CpuLocal::free_raw(reinterpret_cast<VirtAddr>(m_pointer), m_count * sizeof(T));
        m_pointer = nullptr;
        m_count = 0;
    }

    [[nodiscard]]
    auto count() const -> usize {
        return m_count;
    }

    explicit operator bool() const {
        return m_pointer != nullptr;
    }

  private:
    USTL_NOCOPYABLE(PerCpu);
    T *m_pointer;
    usize m_count;
};

template <typename T>
auto CpuLocal::allocate(usize n, usize align) -> PerCpu<T> {
    auto const canonical = Self::allocate_raw(n * sizeof(T), align);
    if (!canonical) {
        return PerCpu<T>();
    }

    auto *pointer = reinterpret_cast<T *>(canonical);
    for (auto cpunum = CpuNum(0); cpunum < MAX_CPU; ++cpunum) {
        auto *base = &Self::access(*pointer, cpunum);
        for (auto i = usize(0); i < n; ++i) {
            new (static_cast<void *>(base + i)) T();
        }
    }

    return PerCpu<T>(pointer, n);
}

template <typename T, typename... Options>
class CpuLocal::Current {
  public:
    FORCE_INLINE
    static auto get() -> T & {
        return CpuLocal::access(g_current_instance);
    }

    FORCE_INLINE
    static auto get(CpuNum cpunum) -> T & {
        return CpuLocal::access(g_current_instance, cpunum);
    }

    FORCE_INLINE
    static auto set(T const &t) -> void {
        if constexpr (sizeof(t) <= sizeof(usize)) {
            CpuLocal::write(g_current_instance, t);
        } else {
            CpuLocal::access(g_current_instance) = t;
        }
    }

    FORCE_INLINE
    static auto set(T const &t, CpuNum cpunum) -> void {
        CpuLocal::access(g_current_instance, cpunum) = t;
    }

  protected:
    static T g_current_instance;
};

template <typename T, typename... Options>
CPU_LOCAL inline T CpuLocal::Current<T, Options...>::g_current_instance;

} // namespace ours::mm

#endif // #ifndef OURS_MM_CPU_LOCAL_HPP
