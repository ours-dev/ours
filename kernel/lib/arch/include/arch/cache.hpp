/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_CACHE_HPP
#define ARCH_CACHE_HPP 1

#include <arch/types.hpp>

namespace arch {

/// Writes every cache line covering [addr, addr + size) back to memory, and
/// returns once the write-back is complete.
///
/// For memory something other than this CPU's caches will read next: a
/// device, or the next boot, since a reset discards dirty lines rather than
/// writing them back. Zircon's arch_clean_cache_range() is the same call.
auto clean_cache_range(void const *addr, usize size) noexcept -> void;

} // namespace arch

// See arch/tick.hpp for why the architecture completion is pulled in here
// rather than left for each caller to include separately.
#if defined(ARCH_X86)
#include <arch/x86/cache.hpp>
#elif defined(ARCH_ARM)
#include <arch/arm/cache.hpp>
#elif defined(ARCH_RISCV)
#include <arch/riscv/cache.hpp>
#endif

#endif // ARCH_CACHE_HPP
