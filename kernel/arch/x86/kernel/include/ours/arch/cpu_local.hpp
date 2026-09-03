/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_CPU_LOCAL_HPP
#define OURS_ARCH_CPU_LOCAL_HPP 1

#include <arch/x86/fsgs.hpp>
#include <arch/x86/msr.hpp>
#include <ours/compiler.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/types.hpp>
#include <ustl/traits/integral.hpp>
#include <ustl/traits/ptr.hpp>

namespace ours {
struct ArchCpuLocal {
    constexpr static usize DYN_FIRST_CHUNK_SIZE = KB(16);

    constexpr static usize UNIT_ALIGN = PAGE_SIZE;

    /// Whether this architecture can spare a contiguous virtual window for the
    /// per-CPU units (see ARCH_CPU_LOCAL_VIRT_BASE).
    ///
    /// True here because a 64-bit kernel address space has room to burn. An
    /// architecture that says false gets the embedded layout instead: units come
    /// straight out of the physmap at unrelated addresses, which costs no address
    /// space but fixes the dynamic area at DYN_FIRST_CHUNK_SIZE forever.
    constexpr static bool HAS_RESERVED_WINDOW = true;

    /// `offset` is `CpuLocal::base(cpu) - CpuLocal::origin()`, not the unit
    /// base itself: %gs supplies only the distance, and the displacement of
    /// every access supplies the object's address.
    template <typename T>
    FORCE_INLINE static auto install(T offset) -> void {
        static_assert(ustl::traits::IsIntegralV<T> || ustl::traits::IsPtrV<T>);

        using namespace arch;
        ::arch::MsrIo::write(::arch::MsrRegAddr::IA32GsBase, offset);
    }

    /// `handle` is the object's address in the linker template -- for a
    /// `CPU_LOCAL` variable, simply `&var`.
    template <typename Integer>
    FORCE_INLINE static auto read(VirtAddr handle) -> Integer {
        return ::arch::read_gs_offset<Integer>(handle);
    }

    template <typename Integer>
    FORCE_INLINE static auto write(VirtAddr handle, Integer value) -> void {
        ::arch::write_gs_offset<Integer>(handle, value);
    }
};

} // namespace ours

#endif // #ifndef OURS_ARCH_CPU_LOCAL_HPP
