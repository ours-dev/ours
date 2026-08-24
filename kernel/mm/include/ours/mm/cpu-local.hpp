/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_CPU_LOCAL_HPP
#define OURS_MM_CPU_LOCAL_HPP 1

#include <ours/arch/cpu-local.hpp>
#include <ours/assert.hpp>
#include <ours/cpu-mask.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/cpu-local-defs.hpp>
#include <ustl/array.hpp>
#include <ustl/function/invoke.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/util/nonconstructible.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::mm {
class CpuLocal {
    using Self = CpuLocal;

  public:
    template <typename T>
    class PerCpu;

    template <typename T, typename... Options>
    class Current;

    static auto init_early() -> void;

    static auto init() -> void;

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
        return *reinterpret_cast<T *>(reinterpret_cast<u8 *>(&t) + Self::g_cpu_offsets[cpunum]);
    }

    template <typename T, typename F>
    FORCE_INLINE static auto with_current(T &t, F &&f)
        -> ustl::traits::InvokeResultT<F, CpuNum, T &> {
        auto const cpu = Cpu::id();
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
    static auto translate(VirtAddr) -> VirtAddr;

    static auto bootstrap(CpuMask const &cpus) -> void;

    template <typename T>
    static auto allocate(usize n, usize align) -> PerCpu<T>;

    template <typename T>
    static auto free(PerCpu<T>) -> void;

  private:
    enum class State : u8 {
        Offline,
        Online,
        NumStates,
    };

    class Dyn;
    class Mapper;

    static ustl::Array<isize, MAX_CPU> g_cpu_offsets;
    static ustl::Array<CpuMask, usize(State::NumStates)> g_states;
    static isize g_current_cpu_offset;
};

template <typename T>
class CpuLocal::PerCpu {
    using Self = PerCpu;

  public:
    PerCpu();
    PerCpu(Self &&);
    ~PerCpu();

    FORCE_INLINE
    auto ptr_mut() -> T *;

    FORCE_INLINE
    auto ref_mut() -> T *;

    template <typename F>
    FORCE_INLINE auto with_current(F &&f) -> ustl::traits::InvokeResultT<F, CpuNum, T &>;

    template <typename F>
    FORCE_INLINE auto for_each(F &&f) -> ustl::traits::InvokeResultT<F, CpuNum, T &>;

    auto operator=(Self &&) -> Self & = default;

    operator bool() {
        return m_pointer == nullptr;
    }

  private:
    USTL_NOCOPYABLE(PerCpu);
    T *m_pointer;
};

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
