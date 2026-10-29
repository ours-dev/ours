// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers
#ifndef ARCH_RISCV_CACHE_HPP
#define ARCH_RISCV_CACHE_HPP 1

#include <arch/types.hpp>

// Only ever reached via <arch/cache.hpp>'s own #include -- don't include this
// file directly. clean_cache_range() has no riscv definition yet: nothing
// built for riscv writes memory back for a device or for the next boot.

namespace arch {

/// fence.i (Zifencei): a riscv hart's instruction fetches need not see its own
/// earlier stores until it runs one, and after it they see every one of them.
/// The fence covers all of memory, not a range, so the range goes unused.
inline auto sync_instruction_fetch_range(void const * /*addr*/, usize /*size*/) noexcept -> void {
    asm volatile("fence.i" ::: "memory");
}

} // namespace arch

#endif // ARCH_RISCV_CACHE_HPP
