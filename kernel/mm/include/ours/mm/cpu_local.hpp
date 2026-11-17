/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_CPU_LOCAL_HPP
#define OURS_MM_CPU_LOCAL_HPP 1

#include <ours/arch/cpu_local.hpp>
#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local_defs.hpp>
#include <ours/task/types.hpp>

#include <array>
#include <new>
#include <tuple>
#include <type_traits>

#include <arch/auto_interrupt_disabler.hpp>
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
/// `CPU_OFFSETS[cpu]` -- is how far that CPU's storage sits from the template.
/// The sum always lands inside live storage -- the static block, or one of the
/// dynamic chunks. `holds()` states that, and debug builds check it on every
/// resolve.
///
/// A `CPU_LOCAL` variable is emitted once, by the linker, and its address *is*
/// its handle: nothing needs to be computed for the static case. The dynamic
/// allocator hands out handles in the same range so that static and dynamic
/// objects are addressed by the same single addition.
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
    /// range, which is never near zero, so null is free to mean empty.
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
        return handle + usize(Self::offset_of(cpu));
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

    class ScopedInstall;

    template <typename T, typename... Options>
    class Pinned;

    /// The two below take a CPU_LOCAL variable and reach *this CPU's copy*.
    /// The x86 contract is that %gs holds `offset(cpu)`, so the displacement is
    /// the variable's own address -- its handle -- and nothing has to be
    /// subtracted first.
    template <typename T>
    FORCE_INLINE
    static auto read(T &t) -> T {
        static_assert(sizeof(t) <= sizeof(usize));
        return ArchCpuLocal::read<T>(VirtAddr{reinterpret_cast<usize>(&t)});
    }

    template <typename T>
    FORCE_INLINE
    static auto write(T &t, T value) -> void {
        static_assert(sizeof(t) <= sizeof(usize));
        return ArchCpuLocal::write<T>(VirtAddr{reinterpret_cast<usize>(&t)}, value);
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
    /// then treat the result as unrelated to `t`.
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
        return *static_cast<T *>(Self::relocate(&t, Self::offset_of(cpunum)));
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

    /// Every CPU slot that has storage, in order.
    ///
    /// Not Cpu::online_mask(), which is what this used to walk. Two reasons,
    /// and either alone settles it:
    ///
    /// A CPU can have storage and not be in that mask. init() gives the boot
    /// CPU its unit, but Cpu::set_state(Online) does not run until the end of
    /// init_arch_early() -- so between those two points walking the online mask
    /// visits *nothing* and returns quietly, and MemCache::init() runs inside
    /// that window. A loop that silently does nothing is worse than one that
    /// faults.
    ///
    /// And even once everything is up, the online mask is the wrong question.
    /// These walks exist to reach every copy of an object -- summing a counter,
    /// draining a cache -- and a CPU that went offline still has its copy, with
    /// whatever it accumulated in it. Skipping it loses data, which is why a
    /// per-CPU sum walks every CPU that may have a copy, not only the online
    /// ones.
    ///
    /// The predicate is "has storage" rather than possible_mask() because
    /// possible_mask() is filled from the MADT, which is parsed after
    /// init_pmm() -- it is not yet meaningful when the first per-CPU walks
    /// happen. init() builds a unit for all MAX_CPU slots for the same reason,
    /// and allocate()/reset() already construct and destroy across the same
    /// range, so this matches what the rest of the class does.
    template <typename F>
    FORCE_INLINE
    static auto for_each_unit(F &&f) -> void {
        for (auto cpunum = CpuNum(0); cpunum < MAX_CPU; ++cpunum) {
            if (Self::CPU_OFFSETS[cpunum] == 0) {
                continue;
            }
            ustl::function::invoke(f, cpunum);
        }
    }

    /// For each instance on different CPU.
    template <typename T, typename F>
    FORCE_INLINE
    static auto for_each(T &t, F &&f) -> void {
        return Self::for_each_unit([&t, &f](CpuNum cpunum) {
            auto &current = Self::access(t, cpunum);
            ustl::function::invoke(f, cpunum, current);
        });
    }

    template <typename U, typename T, typename F>
    FORCE_INLINE
    static auto fold(T &t, U acc, F &&f) -> U {
        Self::for_each_unit([&acc, &t, &f](CpuNum cpunum) {
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

    /// `cpunum`'s offset, checked.
    ///
    /// A CPU with no unit has offset zero -- init_early() clears the table and
    /// only fills in the boot CPU -- and zero does not fail loudly: it resolves
    /// to the linker template itself. That is the one region nothing may write.
    /// It lives in `.init.data.cpu_local`, so after reclaim_init_area() hands
    /// the `.init` area back it is not even stale defaults any more, it is
    /// memory somebody else owns. And because both the handle and the resulting
    /// address are perfectly legal, holds() cannot see it and nothing faults.
    ///
    /// Checked against the offset rather than has_unit(): has_unit() says a CPU
    /// has *attached* to its unit, which is a later event than the unit
    /// existing. init() fills every offset in one pass and then writes each
    /// unit's own bookkeeping through access() in the next, long before any of
    /// those CPUs attach -- asserting on has_unit() there would fire on the
    /// boot path.
    FORCE_INLINE
    static auto offset_of(CpuNum cpunum) -> isize {
        DEBUG_ASSERT(cpunum < MAX_CPU, "Invalid CPU Number");
        DEBUG_ASSERT(Self::CPU_OFFSETS[cpunum] != 0,
                     "CPU[{}] has no per-CPU unit; resolving would land in the template", cpunum);
        return Self::CPU_OFFSETS[cpunum];
    }

    /// Binds the calling CPU to `cpunum`'s unit. `offset` is what the base
    /// register is loaded with.
    static auto attach_to_unit(CpuNum cpunum, isize offset) -> void;

    /// Reserves `size` bytes on every CPU. Returns the handle naming them, or
    /// `NOWHERE` when the dynamic area is exhausted.
    static auto allocate_raw(usize size, usize align) -> VirtAddr;

    static auto free_raw(VirtAddr handle, usize size) -> void;

    /// What a Pinned holds for the option |Option|, as `Type`. Specialised
    /// after Pinned, once per option; left undefined otherwise, so an option
    /// with no saver fails to compile instead of saving nothing.
    template <typename Option>
    struct PinnedSaver;

    static Mapper *MAPPER;
    static Dyn *DYN;

    static std::array<isize, MAX_CPU> CPU_OFFSETS;
    static std::array<CpuMask, usize(State::NumStates)> STATES;
    static isize CURRENT_CPU_OFFSET;
    static CpuNum CURRENT_CPUNUM;
};

/// Points the calling CPU's base register at `cpu`'s unit for the lifetime of
/// the object, and puts back whatever it held before.
///
/// For the entry paths that cannot trust the base register. On x86 an NMI or
/// #MC can land between SYSCALL and its swapgs, or between the exit swapgs and
/// SYSRET -- and in the same spots around every interrupt taken from ring 3.
/// There the saved CS already says ring 0 while %gs.base is still the user's,
/// so "swapgs if we came from user mode" does the wrong thing, and every
/// per-CPU access in the handler lands wherever the user pointed %gs. With
/// FSGSBASE that is anywhere: ring 3 sets it with a plain wrgsbase. So those
/// paths do not ask what %gs holds. The caller works out which CPU it is on
/// without it -- on x86 from TR, which user mode cannot load -- and hands the
/// number in here.
///
/// Putting back the old value, rather than deciding what ought to be there,
/// is what lets one path serve every window: whether the interrupted code was in
/// the kernel with the kernel's %gs, or halfway through a swapgs pair with the
/// user's, it gets back exactly what it had, and its own swapgs still pairs.
/// The other half of the pair (the swapped-out value in KERNEL_GS_BASE) is
/// never touched.
///
/// Neither end touches per-CPU storage. The constructor reads CPU_OFFSETS by
/// its ordinary address and the rest is the register itself, so the object is
/// usable while %gs is still wrong -- which is the point. It reads the table
/// directly rather than through offset_of(), whose "this CPU has a unit"
/// assertion is false before CpuLocal::init_early(), when the boot CPU runs on
/// the template with offset 0 and an NMI is no less possible.
class CpuLocal::ScopedInstall {
  public:
    FORCE_INLINE
    explicit ScopedInstall(CpuNum cpu)
        : m_saved(ArchCpuLocal::installed()) {
        DEBUG_ASSERT(cpu < MAX_CPU, "Invalid CPU number: {}", cpu);
        ArchCpuLocal::install(CpuLocal::CPU_OFFSETS[cpu]);
    }

    FORCE_INLINE
    ~ScopedInstall() {
        ArchCpuLocal::install(m_saved);
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(ScopedInstall);

    /// What the base register held when the object was made.
    isize m_saved;
};

/// Owning handle to a per-CPU allocation.
///
/// Holds a handle, never a usable pointer -- the same handle names the objects
/// on every CPU, and there is no single address that would be right for more
/// than one of them. Nothing has to stop callers dereferencing it: the type
/// simply never offers a way to.
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
    /// `current_cpu()` away.
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
        CpuLocal::for_each_unit(
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

/// The calling CPU's copy of a per-CPU object, for a scope in which the
/// caller cannot be moved off this CPU or have that copy changed under it.
///
/// access(), with_current(), current_cpu() and PerCpu::ref_mut() work out
/// which CPU is running once, when they are called, and hand the answer back.
/// Nothing keeps it true afterwards. The kernel preempts on interrupt exit --
/// arch_handle_exception() calls Thread::Current::preempt() -- so between the
/// call and the last use of its result the thread can be switched out, and
/// then either resume on another CPU still holding the first one's copy,
/// racing that CPU's own code on it, or resume where it was to find that the
/// thread which ran in between changed the copy under it. Protecting the
/// scope afterwards does not help: the resolve itself is the moment that has
/// to be covered. MemCache::allocate() gets it right by hand -- an
/// AutoPreemptDisabler first, then ptr_mut() -- and nothing but care keeps
/// the next caller from writing the two lines the other way round.
///
/// This object does both in the one order that works, and does not let them
/// be separated. The savers are its first member, so interrupts are off or
/// preemption is held -- whichever |Options| ask for -- before the
/// constructor resolves anything, and they are destroyed last, once the
/// reference is gone. There is no way to get the reference without the
/// protection.
///
/// |Options| are the tags a lock's Guard takes, from <ours/lockdep.hpp>,
/// meaning what they mean on a spin lock, and at least one is required for
/// the reason Guard<SpinLock> requires one: without an option this would be
/// access() with extra steps.
///
///   - PreemptDisable: only threads touch the object. Other threads on this
///     CPU are held off; interrupts still arrive. It goes through the current
///     thread's PreemptionState, so it cannot be used before the first
///     thread exists.
///   - IrqSave: an interrupt handler touches it too. Interrupts off also
///     means no preemption, and this is the one option usable before the
///     first thread exists.
///   - IrqSaveAndPreemptDisable: as IrqSave, for a scope that may wake a
///     thread, so that the reschedule the wake asks for waits for the scope
///     to end -- the same rule spin_lock.hpp gives for its Guard.
///
/// Two things are outside what it can cover.
///
///   - Other CPUs. access(t, cpu), for_each() and fold() reach this copy from
///     elsewhere without asking. An object they write still needs a lock or
///     atomics; pinning only settles which copy is *this* CPU's.
///   - Blocking. Holding preemption off does not stop a thread that sleeps
///     from switching away (see AutoPreemptDisabler), and it may be woken on
///     another CPU. Debug builds check on the way out that the thread is still
///     on the CPU it was pinned to, which catches that after the fact.
///
/// And not every per-CPU read needs it. A value that is the same for the
/// calling thread whichever CPU it reads it on -- the current thread pointer
/// is the case in point: every CPU's slot holds whoever is running there --
/// needs only to be read in one instruction, with CpuLocal::read() -- which
/// is what Current<T>::get() does for a T that fits in a register. Pinning
/// is for a copy the caller goes on using, where the CPU it belongs to
/// matters.
template <typename T, typename... Options>
class CpuLocal::Pinned {
    static_assert(sizeof...(Options) > 0,
                  "CpuLocal::Pinned needs an option: PreemptDisable, IrqSave or "
                  "IrqSaveAndPreemptDisable; with none it protects nothing");

  public:
    /// The calling CPU's copy of a CPU_LOCAL variable.
    FORCE_INLINE
    explicit Pinned(T &var)
        : m_savers(), m_cpu(CpuLocal::current_cpu()), m_object(&CpuLocal::access(var, m_cpu)) {}

    /// The calling CPU's copy of a dynamic allocation.
    FORCE_INLINE
    explicit Pinned(PerCpu<T> &percpu)
        : m_savers(), m_cpu(CpuLocal::current_cpu()), m_object(&percpu.ref_mut(m_cpu)) {}

    /// The body runs before any member is destroyed, so the check is made
    /// while the savers are still holding: the CPU seen here is the one the
    /// whole scope ran on unless the scope itself gave the CPU away.
    FORCE_INLINE
    ~Pinned() {
        DEBUG_ASSERT(CpuLocal::current_cpu() == m_cpu,
                     "Pinned to CPU[{}] but leaving on CPU[{}]: the scope blocked", m_cpu,
                     CpuLocal::current_cpu());
    }

    FORCE_INLINE
    auto operator*() const -> T & {
        return *m_object;
    }

    FORCE_INLINE
    auto operator->() const -> T * {
        return m_object;
    }

    /// The CPU whose copy this is, which is also the CPU the caller is on for
    /// as long as the object lives.
    [[nodiscard]]
    FORCE_INLINE
    auto cpu() const -> CpuNum {
        return m_cpu;
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(Pinned);

    /// Declared first because members are initialised in declaration order
    /// and destroyed in reverse: this is what puts the savers' construction
    /// ahead of the resolve below and their destruction after everything
    /// else. Moving it down would resolve with nothing held.
    ///
    /// One saver per option, from CpuLocal::PinnedSaver.
    [[no_unique_address]]
    std::tuple<typename CpuLocal::PinnedSaver<Options>::Type...> m_savers;

    CpuNum m_cpu;
    T *m_object;
};

/// The savers a CpuLocal::Pinned holds for each option: the same types a
/// Guard<SpinLock, ...> holds for the same tag (see spin_lock.hpp), so an
/// option means one thing whether it guards a lock or pins a CPU.
///
/// Spelled out here rather than obtained from lockdep's own lookup, which is
/// that library's machinery for its Guard and not an interface for others to
/// build on.
///
/// task::AutoPreemptDisabler is incomplete here and cannot be completed --
/// its header includes thread.hpp, which includes this one. Naming it in an
/// alias needs no more than the declaration in <ours/task/types.hpp>, and a
/// Pinned<T, PreemptDisable> needs <ours/task/auto_preempt_disabler.hpp>
/// where it is used, as a Guard<SpinLock, PreemptDisable> does.
///
/// IrqSaveAndPreemptDisable is a tuple for the order it destroys in, the
/// same as SpinLock's: preemption is re-enabled first, so a reschedule that
/// was held back runs with interrupts still off, and interrupts come back
/// last.
template <>
struct CpuLocal::PinnedSaver<IrqSave> {
    using Type = ::arch::AutoInterruptDisabler;
};

template <>
struct CpuLocal::PinnedSaver<PreemptDisable> {
    using Type = task::AutoPreemptDisabler;
};

template <>
struct CpuLocal::PinnedSaver<IrqSaveAndPreemptDisable> {
    using Type = std::tuple<::arch::AutoInterruptDisabler, task::AutoPreemptDisabler>;
};

template <typename T, typename... Options>
class CpuLocal::Current {
  public:
    /// Whether get() and set() read and write T as a value, in one
    /// %gs-relative instruction, rather than reach the object: a scalar that
    /// fits in a register is a value, anything else an object.
    static constexpr bool IS_VALUE = std::is_scalar_v<T> && sizeof(T) <= sizeof(usize);

    /// This CPU's instance: the value when T is a scalar that fits in a
    /// register, a reference to it otherwise.
    ///
    /// Two shapes because the two kinds of T are used in two ways. An object
    /// is something the caller goes on working with, so it gets a reference,
    /// and keeping the caller on this CPU while it does is the caller's job
    /// (CpuLocal::Pinned). A scalar is, in every instance in the tree, a
    /// pointer naming which object is current on this CPU -- the running
    /// Thread, this CPU's PmNode -- read once and used as a value.
    ///
    /// That read has to be one instruction. Through access() it was two: load
    /// %gs:CURRENT_CPU_OFFSET, then load the slot at template + offset. A
    /// thread preempted between them and resumed on another CPU loads the
    /// first CPU's slot, and for Thread::Current that slot by then names
    /// whichever thread the first CPU switched to -- the caller would go on to
    /// raise that thread's preemption count, or run in its address space.
    /// CpuLocal::read() is a single %gs-relative load; a preemption lands
    /// before it or after it, and either way it reads the slot of the CPU the
    /// caller is on at that instant -- for Thread::Current, the caller itself.
    ///
    /// A CPU with nothing to run now takes a queued thread from another
    /// (Scheduler::steal_work_from()), a preempted one included, so a thread
    /// preempted between the two loads can resume on another CPU: the two-load
    /// form is only for a caller that cannot be preempted, with preemption or
    /// interrupts off. Every caller of the one-argument form was read on
    /// 2026-11-05 and is such a caller (ChainLockTransaction::current() has
    /// none to check); the forms built on a CPU read in two steps are not,
    /// and are items 28 to 31 of docs/repairs/261103/kernel-task-sched.md.
    ///
    /// The test is whether T is a scalar, not only whether it fits: a class
    /// that happens to be small is still an object, and a copy of it would
    /// leave every change made through get() on the copy. Size alone once
    /// decided, and TickSched, down to 8 bytes, came back as a copy that its
    /// atomic member refused to make. set() makes the same test to choose
    /// write().
    FORCE_INLINE
    static auto get() -> decltype(auto) {
        if constexpr (IS_VALUE) {
            return CpuLocal::read(CURRENT_INSTANCE);
        } else {
            return CpuLocal::access(CURRENT_INSTANCE);
        }
    }

    FORCE_INLINE
    static auto get(CpuNum cpunum) -> T & {
        return CpuLocal::access(CURRENT_INSTANCE, cpunum);
    }

    FORCE_INLINE
    static auto set(T const &t) -> void {
        if constexpr (IS_VALUE) {
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
