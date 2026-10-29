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
/// writing them back.
auto clean_cache_range(void const *addr, usize size) noexcept -> void;

/// Makes this CPU's earlier stores to [addr, addr + size) visible to its own
/// later instruction fetches, and returns once they are.
///
/// For memory this CPU has just written code into and will execute next, as a
/// loader does with an image before it jumps into it. Not every architecture
/// fetches instructions through a path that sees the CPU's own stores; on one
/// that does not, running the new bytes without this may execute whatever an
/// instruction cache still held for that memory. Only this CPU is covered: a
/// second CPU that is to run the code needs a synchronisation of its own.
auto sync_instruction_fetch_range(void const *addr, usize size) noexcept -> void;

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
