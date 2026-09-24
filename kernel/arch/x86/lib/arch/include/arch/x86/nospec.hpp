/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_NOSPEC_HPP
#define ARCH_X86_NOSPEC_HPP 1

// Only ever reached via <arch/nospec.hpp>'s own #include -- don't include
// this file directly.

namespace arch {

/// |index| if it is below |size|, otherwise 0; see <arch/nospec.hpp>.
///
/// `cmp` computes size - index, and `cmova` moves |index| into the result
/// only when that is "above" -- unsigned greater, so size > index. Until the
/// comparison resolves the move cannot execute, and the result a speculative
/// path consumes is still the 0 it started as. Intel, "Analyzing potential
/// bounds check bypass vulnerabilities", section 5.2 (bounds clipping); AMD,
/// "Software techniques for managing speculation", mitigation V1-2.
[[nodiscard]]
inline auto confine_array_index(usize const index, usize const size) -> usize {
    usize safe_index = 0;
    asm("cmp %1, %2\n\t"
        "cmova %1, %0"
        : "+r"(safe_index)
        : "r"(index), "r"(size)
        : "cc");
    return safe_index;
}

/// |a| if |x| equals |y|, otherwise |b|; see <arch/nospec.hpp>.
///
/// The result starts as |a| and `cmovnz` replaces it with |b| when the
/// comparison found them different -- the same data dependency as above.
[[nodiscard]]
inline auto conditional_select_nospec_eq(usize const x, usize const y, usize const a,
                                         usize const b) -> usize {
    usize select = a;
    asm("cmp %1, %2\n\t"
        "cmovnz %3, %0"
        : "+r"(select)
        : "r"(x), "r"(y), "r"(b)
        : "cc");
    return select;
}

} // namespace arch

#endif // ARCH_X86_NOSPEC_HPP
