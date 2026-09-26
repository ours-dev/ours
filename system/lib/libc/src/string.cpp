/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The C names of mem* and strlen, each forwarding to the generic byte loop
/// of the same name in string_impl.cpp.
///
/// Every definition is WEAK_LINK on purpose. These are the defaults, and an
/// architecture that has something better -- `rep movsb` where the CPU makes
/// it fast, a word-at-a-time strlen -- supplies a strong definition of the
/// same name, and the linker takes it over this one without any other file
/// changing. It is the pattern kernel/arch/x86/mm/srat.cpp already uses to
/// replace the weak acpi_init_numa() stub in kernel/arch/x86/mm/numa.cpp.
/// Until an override exists, the weak definition is simply the only one.
///
/// The forwarding costs a jump: the loops live in another translation unit
/// so that a host test can link them without these names (see
/// string_impl.hpp), and nothing here is built with LTO to inline across
/// the two.
#include <cstring>

#include "string_impl.hpp"
#include <ours/compiler.hpp>
#include <ours/types.hpp>

extern "C" WEAK_LINK
auto memcpy(void *d, void const *s, size_t n) -> void * {
    return ours::libc::memcpy(d, s, n);
}

extern "C" WEAK_LINK
auto memmove(void *d, void const *s, size_t n) -> void * {
    return ours::libc::memmove(d, s, n);
}

extern "C" WEAK_LINK
auto memset(void *s, int c, size_t n) -> void * {
    return ours::libc::memset(s, c, n);
}

extern "C" WEAK_LINK
auto memcmp(void const *x, void const *y, size_t n) -> int {
    return ours::libc::memcmp(x, y, n);
}

/// Not `extern "C"` here: it has C linkage all the same, from the
/// declaration <cstring> brings in above.
///
/// The return type is `void const *` because that is what this tree's
/// freestanding <string.h> declares; C's own memchr() returns `void *` from a
/// `const void *`, and reconciling the two is a separate change to the shared
/// header, not to this definition (docs/repairs/260919/libc.md).
WEAK_LINK
auto memchr(void const *str, int c, size_t n) -> void const * {
    return ours::libc::memchr(str, c, n);
}

extern "C" WEAK_LINK
auto strlen(char const *p) -> size_t {
    return ours::libc::strlen(p);
}
