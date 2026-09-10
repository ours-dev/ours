/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_CPU_LOCAL_HPP
#define OURS_MM_CPU_LOCAL_HPP 1

#include <ours/arch/cpu_local.hpp>
#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/cpu_local_defs.hpp>

#include <array>
#include <new>

#include <ustl/function/invoke.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/util/move.hpp>
#include <ustl/util/nonconstructible.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::mm {
/// Per-CPU storage.
///
/// The whole interface is one equation:
///
///     address = handle + offset(cpu)
///
/// A *handle* names an object. It is the same number on every CPU, it is what
/// callers hold, and it is deliberately not a usable pointer: it points into
/// the linker template, which must never be read or written. `offset(cpu)` --
/// `CPU_OFFSETS[cpu]`, which is Linux's `__per_cpu_offset[]` -- is how far that
/// CPU's storage sits from the template. The sum always lands
/// inside live storage -- the static block, or one of the dynamic chunks.
/// `holds()` states that, and debug builds check it on every resolve.
///
/// A `CPU_LOCAL` variable is emitted once, by the linker, and its address *is*
/// its handle: nothing needs to be computed for the static case. The dynamic
/// allocator hands out handles in the same range so that static and dynamic
/// objects are addressed by the same single addition. This is the scheme Linux
/// uses -- see `__addr_to_pcpu_ptr()` in its `mm/percpu.c`, which converts a
/// real address into exactly this kind of handle.
///
/// Each CPU owns a *unit*: its own copy of the template block, followed by
/// room the dynamic allocator hands out. Units are grouped by NUMA node, so
/// their addresses are unrelated to each other and `offset()` is a lookup, not
/// arithmetic on a stride. Dynamic chunks are separate allocations that
/// reproduce the same unit layout, which is what lets one handle name an
/// object in any of them:
///
///     base(cpu) ---> +-----------------+-------------------+
///                    | copy of the     | dynamic area      |
///                    | linker template |                   |
///                    +-----------------+-------------------+
///                     \__ origin() .. origin() + static_span __/
///
/// On architectures with a base register (%gs on x86) the register holds
/// `offset(cpu)` itself, so a static object resolves in a single instruction:
/// the handle is the variable's own address, which the linker already baked
/// into the instruction stream.
class CpuLocal {
    using Self = CpuLocal;

  public:
    template <typename T>
    class PerCpu;

    template <typename T, typename... Options>
    class Current;

    /// Handle value that names nothing. Handles live in the template's address
    /// range, which is never near zero, so null is free to mean empty -- the
    /// same convention Linux gives its `void __percpu *`.
    constexpr static auto NOWHERE = VirtAddr(0);

    /// Where CPU `cpu`'s storage begins.
    static auto base(CpuNum cpu) -> VirtAddr;

    /// Start of the address range handles live in -- the linker template.
    /// Nothing is read or written through it.
    static auto origin() -> VirtAddr;

    /// Whether `handle` names storage that exists: inside the static block, or
    /// inside a live dynamic chunk. The contract every resolve relies on.
    [[nodiscard]]
    static auto holds(VirtAddr handle) -> bool;

    /// `handle` on the calling CPU. Uses the base register rather than a lookup.
    FORCE_INLINE
    static auto resolve(VirtAddr handle) -> VirtAddr {
        DEBUG_ASSERT(Self::holds(handle), "Resolving a handle that names nothing");
        return handle + Self::read(CURRENT_CPU_OFFSET);
    }

    /// `handle` on `cpu`.
    FORCE_INLINE
    static auto resolve(VirtAddr handle, CpuNum cpu) -> VirtAddr {
        DEBUG_ASSERT(Self::holds(handle), "Resolving a handle that names nothing");
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU Number");
        return handle + VirtAddr(Self::CPU_OFFSETS[cpu]);
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

    /// The two below take a CPU_LOCAL variable and reach *this CPU's copy*.
    /// The x86 contract is that %gs holds `offset(cpu)`, so the displacement is
    /// the variable's own address -- its handle -- and nothing has to be
    /// subtracted first.
    template <typename T>
    FORCE_INLINE
    static auto read(T &t) -> T {
        static_assert(sizeof(t) <= sizeof(usize));
        return ArchCpuLocal::read<T>(reinterpret_cast<VirtAddr>(&t));
    }

    template <typename T>
    FORCE_INLINE
    static auto write(T &t, T value) -> void {
        static_assert(sizeof(t) <= sizeof(usize));
        return ArchCpuLocal::write<T>(reinterpret_cast<VirtAddr>(&t), value);
    }

    /// Takes the address of a CPU_LOCAL variable's *template* copy and returns
    /// the address of a unit's copy, with the compiler unable to tell they are
    /// related.
    ///
    /// The barrier is the whole point, and it is not optional. `&t + offset`
    /// walks out of `t`, and the optimiser is entitled to assume a pointer
    /// derived from an object still designates that object -- so it folds the
    /// arithmetic away and reads the template copy, which is a zeroed image
    /// that nothing ever writes. It really does: before this, `Thread::Current`
    /// wrote through the %gs path (inline asm, opaque) and read back through
    /// here, and clang proved the result null at compile time -- it emitted the
    /// panic branch of `DEBUG_ASSERT(thread)` with no test at all.
    ///
    /// An empty asm with the pointer in and out launders it: the compiler must
    /// then treat the result as unrelated to `t`. This is exactly Linux's
    /// RELOC_HIDE(), used for the same reason in the same place.
    [[nodiscard]]
    FORCE_INLINE
    static auto relocate(void *base, isize offset) -> void * {
        auto *address = static_cast<u8 *>(base) + offset;
        asm("" : "+r"(address));
        return address;
    }

    template <typename T>
    FORCE_INLINE
    static auto access(T &t) -> T & {
        return *static_cast<T *>(Self::relocate(&t, Self::read(CURRENT_CPU_OFFSET)));
    }

    template <typename T>
    FORCE_INLINE
    static auto access(T &t, CpuNum cpunum) -> T & {
        DEBUG_ASSERT(cpunum < MAX_CPU, "Invalid CPU Number");
        return *static_cast<T *>(Self::relocate(&t, Self::CPU_OFFSETS[cpunum]));
    }

    /// The CPU number the caller is running on, read out of its own unit.
    FORCE_INLINE
    static auto current_cpu() -> CpuNum {
        return Self::read(CURRENT_CPUNUM);
    }

    /// Whether `cpu` has a per-CPU unit installed -- that is, whether
    /// `CPU_OFFSETS[cpu]` means anything yet. This is CpuLocal's own
    /// bookkeeping, set for the boot CPU by init_early() and for each secondary
    /// as it comes up, and it is a different question from ours::Cpu's online
    /// mask, which says the CPU has finished its architectural bring-up.
    [[nodiscard]]
    FORCE_INLINE
    static auto has_unit(CpuNum cpu) -> bool {
        return STATES[usize(State::Online)].test(cpu);
    }

    template <typename T, typename F>
    FORCE_INLINE
    static auto with_current(T &t, F &&f) -> ustl::traits::InvokeResultT<F, CpuNum, T &> {
        // current_cpu() rather than Cpu::id(): identical value, but Cpu::id()
        // is now a forwarder to it, and this is the hot path.
        auto const cpu = Self::current_cpu();
        // has_unit(), not Cpu::online_mask(): what this needs is a valid offset
        // for the calling CPU, which is exactly what CpuLocal tracks. The
        // global online mask answers a later question -- the boot CPU is not in
        // it until the end of init_arch_early() -- so asserting on it fired on
        // every per-CPU access made before then, including the first spin lock
        // task::init_early() takes.
        DEBUG_ASSERT(Self::has_unit(cpu), "The calling CPU has no per-CPU unit");
        return ustl::function::invoke(f, cpu, Self::access(t, cpu));
    }

    /// For each instance on different CPU.
    template <typename T, typename F>
    FORCE_INLINE
    static auto for_each(T &t, F &&f) -> void {
        return Cpu::online_mask().for_each([&t, &f](CpuNum cpunum) {
            auto &current = Self::access(t, cpunum);
            ustl::function::invoke(f, cpunum, current);
        });
    }

    template <typename U, typename T, typename F>
    FORCE_INLINE
    static auto fold(T &t, U acc, F &&f) -> U {
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

    /// Opaque here on purpose: both are defined in cpu_local.cpp, so an
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

    /// Binds the calling CPU to `cpunum`'s unit. `offset` is what the base
    /// register is loaded with.
    static auto attach_to_unit(CpuNum cpunum, isize offset) -> void;

    /// Reserves `size` bytes on every CPU. Returns the handle naming them, or
    /// `NOWHERE` when the dynamic area is exhausted.
    static auto allocate_raw(usize size, usize align) -> VirtAddr;

    static auto free_raw(VirtAddr handle, usize size) -> void;

    static Mapper *MAPPER;
    static Dyn *DYN;

    static std::array<isize, MAX_CPU> CPU_OFFSETS;
    static std::array<CpuMask, usize(State::NumStates)> STATES;
    static isize CURRENT_CPU_OFFSET;
    static CpuNum CURRENT_CPUNUM;
};

/// Owning handle to a per-CPU allocation.
///
/// Holds a handle, never a usable pointer -- the same handle names the objects
/// on every CPU, and there is no single address that would be right for more
/// than one of them. Linux relies on sparse and a `__percpu` annotation to stop
/// callers dereferencing the equivalent value; here the type simply never
/// offers a way to.
template <typename T>
class CpuLocal::PerCpu {
    using Self = PerCpu;

  public:
    PerCpu() : m_handle(NOWHERE), m_count(0) {}

    PerCpu(VirtAddr handle, usize count) : m_handle(handle), m_count(count) {}

    PerCpu(Self &&other) : m_handle(other.m_handle), m_count(other.m_count) {
        other.m_handle = NOWHERE;
        other.m_count = 0;
    }

    ~PerCpu() {
        reset();
    }

    auto operator=(Self &&other) -> Self & {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            m_count = other.m_count;
            other.m_handle = NOWHERE;
            other.m_count = 0;
        }
        return *this;
    }

    /// The handle these objects are named by.
    [[nodiscard]]
    auto handle() const -> VirtAddr {
        return m_handle;
    }

    /// The calling CPU's copy.
    FORCE_INLINE
    auto ptr_mut() -> T * {
        return m_handle == NOWHERE ? nullptr : reinterpret_cast<T *>(CpuLocal::resolve(m_handle));
    }

    FORCE_INLINE
    auto ref_mut() -> T & {
        DEBUG_ASSERT(m_handle != NOWHERE, "Dereferencing an empty PerCpu");
        return *reinterpret_cast<T *>(CpuLocal::resolve(m_handle));
    }

    /// `cpunum`'s copy.
    FORCE_INLINE
    auto ref_mut(CpuNum cpunum) -> T & {
        DEBUG_ASSERT(m_handle != NOWHERE, "Dereferencing an empty PerCpu");
        return *reinterpret_cast<T *>(CpuLocal::resolve(m_handle, cpunum));
    }

    /// Unlike the static `CpuLocal::with_current`, the callback takes only the
    /// object: a handle already names one thing, and the CPU number is a
    /// `current_cpu()` away. This is the shape PmZone::alloc_frame() uses.
    template <typename F>
    FORCE_INLINE
    auto with_current(F &&f) -> ustl::traits::InvokeResultT<F, T &> {
        return ustl::function::invoke(f, ref_mut());
    }

    template <typename F>
    FORCE_INLINE
    auto for_each(F &&f) -> void {
        if (m_handle == NOWHERE) {
            return;
        }
        Cpu::online_mask().for_each(
            [this, &f](CpuNum cpunum) { ustl::function::invoke(f, cpunum, ref_mut(cpunum)); });
    }

    /// Destroys every CPU's copy and releases the reservation.
    auto reset() -> void {
        if (m_handle == NOWHERE) {
            return;
        }

        // Mirrors allocate(): every slot was constructed, so every slot is
        // destroyed, independent of which CPUs happen to be possible now.
        for (auto cpunum = CpuNum(0); cpunum < MAX_CPU; ++cpunum) {
            auto *base = reinterpret_cast<T *>(CpuLocal::resolve(m_handle, cpunum));
            for (auto i = m_count; i-- > 0;) {
                base[i].~T();
            }
        }

        CpuLocal::free_raw(m_handle, m_count * sizeof(T));
        m_handle = NOWHERE;
        m_count = 0;
    }

    [[nodiscard]]
    auto count() const -> usize {
        return m_count;
    }

    explicit operator bool() const {
        return m_handle != NOWHERE;
    }

  private:
    USTL_NOCOPYABLE(PerCpu);
    VirtAddr m_handle;
    usize m_count;
};

template <typename T>
auto CpuLocal::allocate(usize n, usize align) -> PerCpu<T> {
    auto const handle = Self::allocate_raw(n * sizeof(T), align);
    if (Self::NOWHERE == handle) {
        return PerCpu<T>();
    }

    for (auto cpunum = CpuNum(0); cpunum < MAX_CPU; ++cpunum) {
        auto *base = reinterpret_cast<T *>(Self::resolve(handle, cpunum));
        for (auto i = usize(0); i < n; ++i) {
            new (static_cast<void *>(base + i)) T();
        }
    }

    return PerCpu<T>(handle, n);
}

template <typename T, typename... Options>
class CpuLocal::Current {
  public:
    FORCE_INLINE
    static auto get() -> T & {
        return CpuLocal::access(CURRENT_INSTANCE);
    }

    FORCE_INLINE
    static auto get(CpuNum cpunum) -> T & {
        return CpuLocal::access(CURRENT_INSTANCE, cpunum);
    }

    FORCE_INLINE
    static auto set(T const &t) -> void {
        if constexpr (sizeof(t) <= sizeof(usize)) {
            CpuLocal::write(CURRENT_INSTANCE, t);
        } else {
            CpuLocal::access(CURRENT_INSTANCE) = t;
        }
    }

    FORCE_INLINE
    static auto set(T const &t, CpuNum cpunum) -> void {
        CpuLocal::access(CURRENT_INSTANCE, cpunum) = t;
    }

  protected:
    static T CURRENT_INSTANCE;
};

template <typename T, typename... Options>
CPU_LOCAL inline T CpuLocal::Current<T, Options...>::CURRENT_INSTANCE;

} // namespace ours::mm

#endif // #ifndef OURS_MM_CPU_LOCAL_HPP
