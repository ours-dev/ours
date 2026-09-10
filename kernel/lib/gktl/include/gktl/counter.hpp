/// Copyright(C) 2024 smallhuazi
///
/// This program is free software; you can redistribute it and/or modify
/// it under the terms of the GNU General Public License as published
/// by the Free Software Foundation; either version 2 of the License, or
/// (at your option) any later version.  ///
/// For additional information, please refer to the following website:
/// https://opensource.org/license/gpl-2-0
///
#ifndef GKTL_COUNTER_HPP
#define GKTL_COUNTER_HPP 1

#include <gktl/types.hpp>

#include <limits>

#include <ours/mm/cpu_local.hpp>
#include <ustl/sync/atomic.hpp>

namespace gktl {
class Counter {
  public:
    enum class Type : u8 {
        Sum,
        Min,
        Max,
    };

    struct Descriptor {
        using Self = Descriptor;

        char name[63];
        Type type;

        static Self const GLOBAL_DESC_START[] LINK_NAME("__gktl_counter_desc_start");
        static Self const GLOBAL_DESC_END[] LINK_NAME("__gktl_counter_desc_end");
    };

    constexpr Counter(Descriptor const *descriptor) : m_descriptor(descriptor) {}

    ~Counter() = default;

    auto sum_across_all_cpus() -> isize {
        return operate_across_all_cpus(isize(0), [](isize x, isize y) { return x + y; });
    }

    auto max_across_all_cpus() -> isize {
        return operate_across_all_cpus(std::numeric_limits<isize>::min(), std::greater<isize>());
    }

    auto min_across_all_cpus() -> isize {
        return operate_across_all_cpus(std::numeric_limits<isize>::max(), std::less<isize>());
    }

    auto set(isize value) const -> void {
        ours::mm::CpuLocal::with_current(slot(), [value](ours::CpuNum, isize &slot) {
            ustl::sync::AtomicRef slot_ref{slot};
            slot_ref.store(value, ustl::sync::MemoryOrder::Relaxed);
        });
    }

    auto get() const -> isize {
        return ours::mm::CpuLocal::with_current(slot(), [](ours::CpuNum, isize &slot) {
            ustl::sync::AtomicRef slot_ref{slot};
            return slot_ref.load(ustl::sync::MemoryOrder::Relaxed);
        });
    }

    auto add(isize delta) const -> void {
        ours::mm::CpuLocal::with_current(slot(), [delta](ours::CpuNum, isize &slot) {
            ustl::sync::AtomicRef slot_ref{slot};
            slot_ref.fetch_add(delta, ustl::sync::MemoryOrder::Relaxed);
        });
    }

    auto max(isize value) const -> isize {
        return ours::mm::CpuLocal::with_current(slot(), [value](ours::CpuNum, isize &slot) {
            ustl::sync::AtomicRef slot_ref{slot};
            auto const curr_value = slot_ref.load(ustl::sync::MemoryOrder::Relaxed);
            if (value > curr_value) {
                slot_ref.store(value, ustl::sync::MemoryOrder::Relaxed);
                return value;
            }

            return curr_value;
        });
    }

    auto min(isize value) const -> isize {
        return ours::mm::CpuLocal::with_current(slot(), [value](ours::CpuNum, isize &slot) {
            ustl::sync::AtomicRef slot_ref{slot};
            auto const curr_value = slot_ref.load(ustl::sync::MemoryOrder::Relaxed);
            if (value < curr_value) {
                slot_ref.store(value, ustl::sync::MemoryOrder::Relaxed);
                return value;
            }

            return curr_value;
        });
    }

  private:
    template <typename U, typename F>
    FORCE_INLINE
    auto operate_across_all_cpus(U initial, F &&f) -> ustl::traits::InvokeResultT<F, U, isize> {
        return ours::mm::CpuLocal::fold(slot(), initial, [&](ours::CpuNum, isize &counter, U acc) {
            ustl::sync::AtomicRef slot{counter};
            return ustl::function::invoke(ustl::move(f), acc,
                                          slot.load(ustl::sync::MemoryOrder::Relaxed));
        });
    }

    auto slot() const -> isize & {
        return COUNTERS_START[index()];
    }
    auto index() const -> usize {
        return m_descriptor - Descriptor::GLOBAL_DESC_START;
    }

    Descriptor const *m_descriptor;
    static isize COUNTERS_START[] LINK_NAME("__gktl_counter_start");
    static isize COUNTERS_END[] LINK_NAME("__gktl_counter_end");
};

} // namespace gktl

/// @param VAR
/// @param NAME
/// @param TYPE Sum, Min or Max.
#define GKTL_COUNTER_DEFINE(VAR, NAME, TYPE)                                                       \
    LINK_SECTION(".gktl.counter.descs")                                                            \
    USED constinit static gktl::Counter::Descriptor const COUNTER_DESCRIPTOR_##VAR = {             \
        .name = NAME, .type = gktl::Counter::Type::TYPE};                                          \
                                                                                                   \
    CPU_LOCAL_SPECIAL(".gktl.counter.storage")                                                     \
    constinit static isize COUNTER_STORAGE_##VAR;                                                  \
                                                                                                   \
    [[maybe_unused]]                                                                               \
    static gktl::Counter VAR{&COUNTER_DESCRIPTOR_##VAR};

#define GKTL_COUNTER(VAR, NAME) GKTL_COUNTER_DEFINE(VAR, NAME, Sum)
#include <algorithm>
#include <functional>

#endif // #ifndef GKTL_COUNTER_HPP
