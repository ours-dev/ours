/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_TLB_HPP
#define ARCH_X86_TLB_HPP 1

#include <ours/compiler.hpp>

#include <arch/types.hpp>
#include <arch/x86/system.hpp>

// Only ever reached via <arch/tlb.hpp>'s own #include — don't include this
// file directly.

namespace arch {

FORCE_INLINE
inline auto x86_invlpg(VirtAddr address) -> void {
    asm volatile("invlpg (%0)" : : "r"(address) : "memory");
}

inline auto tlb_invalidate_all() -> void {
    // Reloading CR3 flushes every non-global TLB entry; this is the
    // fallback path when INVPCID isn't available (see x86_invpcid_all()).
    Cr3::write(Cr3::read());
}

FORCE_INLINE
inline auto x86_invpcid_all() -> void {
    struct alignas(16) InvpcidDescriptor {
        u64 pcid;
        u64 addr;
    } desc{0, 0};
    // INVPCID type 2: invalidate all contexts, including global
    // translations. Caller must have already checked
    // CpuFeatureType::InvPcid.
    usize const type = 2;
    asm volatile("invpcid %0, %1" : : "m"(desc), "r"(type) : "memory");
}

} // namespace arch

#endif // ARCH_X86_TLB_HPP
