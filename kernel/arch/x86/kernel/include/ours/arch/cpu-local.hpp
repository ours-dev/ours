/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_CPU_LOCAL_HPP
#define OURS_ARCH_CPU_LOCAL_HPP 1

#include <arch/x86/fsgs.hpp>
#include <arch/x86/msr.hpp>
#include <ours/compiler.hpp>
#include <ours/mm/cfg.hpp>
#include <ustl/traits/integral.hpp>
#include <ustl/traits/ptr.hpp>

namespace ours {
struct ArchCpuLocal {
    constexpr static usize const kDynFirstChunkSize = KB(16);

    constexpr static usize const kUnitAlign = PAGE_SIZE;

    template <typename T>
    FORCE_INLINE static auto install(T base) -> void {
        static_assert(ustl::traits::IsIntegralV<T> || ustl::traits::IsPtrV<T>);

        using namespace arch;
        MsrIo::write(MsrRegAddr::IA32GsBase, base);
    }

    template <typename Integer>
    FORCE_INLINE static auto read(usize offset) -> Integer {
        return arch::read_gs_offset<Integer>(offset);
    }

    template <typename Integer>
    FORCE_INLINE static auto write(usize offset, Integer value) -> void {
        arch::write_gs_offset<Integer>(offset, value);
    }
};

} // namespace ours

#endif // #ifndef OURS_ARCH_CPU_LOCAL_HPP
