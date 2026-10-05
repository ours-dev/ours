// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_CACHE_HPP
#define ARCH_X86_CACHE_HPP 1

#include <arch/types.hpp>

// The x86 cache maintenance interface. <arch/cache.hpp> declares it; mm still
// includes this file directly from before that header existed.

namespace arch {

/// CLFLUSH over every line of the range, between two MFENCEs: the first
/// orders the stores that filled the range before the flushes, the second
/// waits for the flushes to finish. CLFLUSH also evicts the line, which a
/// pure write-back (CLWB) would not, but CLWB is not on the Nehalem baseline
/// this kernel targets.
///
/// The step is 64 bytes, the line size of every x86-64 CPU to date. A larger
/// line would only be flushed more than once. CPUID leaf 1 reports the real
/// size (EBX bits 15:8, in 8-byte units) should it ever be needed.
inline auto clean_cache_range(void const *addr, usize size) noexcept -> void {
    constexpr auto LINE_BYTES = usize{64};
    auto const first = reinterpret_cast<usize>(addr) & ~(LINE_BYTES - 1);
    auto const end = reinterpret_cast<usize>(addr) + size;
    asm volatile("mfence" ::: "memory");
    for (auto line = first; line < end; line += LINE_BYTES) {
        asm volatile("clflush (%0)" ::"r"(line) : "memory");
    }
    asm volatile("mfence" ::: "memory");
}

} // namespace arch

#endif // ARCH_X86_CACHE_HPP
