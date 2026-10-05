/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ORT_SPIN_HINT_HPP
#define ORT_SPIN_HINT_HPP 1

namespace ort {
/// Tells the CPU the calling thread is spinning: PAUSE on x86, YIELD on arm,
/// the pause hint on riscv. It is also a side effect the optimiser must keep.
/// C++ lets a compiler assume that a loop with no side effects ends, and
/// clang at -O2 deletes `for (;;) {}` outright, so a loop meant to spin for
/// ever calls this rather than leave its body empty. User code writes these
/// instructions here and nowhere else; the kernel's own is arch::spin_hint().
inline auto spin_hint() noexcept -> void {
#if defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__riscv)
    asm volatile(".4byte 0x0100000f" ::: "memory");
#else
#error "ort::spin_hint() does not know this architecture"
#endif
}
} // namespace ort

#endif // ORT_SPIN_HINT_HPP
