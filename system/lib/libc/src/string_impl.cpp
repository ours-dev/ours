/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The generic mem* and strlen: byte loops, correct everywhere and fast
/// nowhere. string.cpp exports them under their C names.
///
/// This file must be compiled with -fno-builtin (BUILD.gn gives it that in
/// every toolchain). Without it LLVM's loop-idiom pass turns the fill loop
/// below into a call to memset and the counting loop into a call to strlen
/// (the copy loops it leaves alone, only because their source and
/// destination may overlap). It skips a function that is itself *named*
/// memset or strlen, but not ours::libc::memset. In a target build the call
/// would go out through the C name in string.cpp and straight back into this
/// loop, forever; in the host test build it would reach the host's libc, and
/// the tests would be checking the host's memset instead of this one. The
/// C-named bodies these loops used to be were safe for a different reason:
/// every toolchain that builds them is -ffreestanding, which implies
/// -fno-builtin.
#include "string_impl.hpp"

namespace ours::libc {
auto memcpy(void *const dst, void const *const src, usize const n) -> void * {
    auto *const d = static_cast<char *>(dst);
    auto const *const s = static_cast<char const *>(src);
    for (usize i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

/// Forwards when the source is above the destination and backwards
/// otherwise, so that an overlapping source byte is always read before the
/// copy overwrites it. Equal pointers take the backward loop, which copies
/// every byte onto itself.
auto memmove(void *const dst, void const *const src, usize const n) -> void * {
    auto *const d = static_cast<char *>(dst);
    auto const *const s = static_cast<char const *>(src);
    if (usize(s) > usize(d)) {
        for (usize i = 0; i < n; ++i) {
            d[i] = s[i];
        }
    } else {
        for (usize i = n; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

/// Only the low byte of |c| is stored, as C requires (converted to unsigned
/// char, C17 7.24.6.1).
auto memset(void *const dst, int const c, usize const n) -> void * {
    auto *const d = static_cast<u8 *>(dst);
    for (usize i = 0; i < n; ++i) {
        d[i] = static_cast<u8>(c);
    }
    return dst;
}

/// Compares the bytes as unsigned char, as C requires (C17 7.24.4), so that
/// the sign of the answer orders the buffers by byte value.
///
/// It used to read them as `char`, which is signed on x86-64: any byte of
/// 0x80 or above counted as negative, and memcmp({0x80}, {0x01}, 1) answered
/// -129 where it must be positive. Equality came out right -- the difference
/// is still non-zero -- and every caller in the tree only asks for equality
/// (ELF magic, symbol names), which is why it went unnoticed. The caller that
/// asks for order is libc++: std::string_view's compare() and operator< go
/// through char_traits<char>::compare, which is this function at run time,
/// while the same comparison evaluated at compile time orders by unsigned
/// char. The two would disagree on any name with a byte past ASCII, and a
/// lookup in a table sorted one way and searched the other would miss
/// without an error.
auto memcmp(void const *const lhs, void const *const rhs, usize const n) -> int {
    auto const *const p1 = static_cast<u8 const *>(lhs);
    auto const *const p2 = static_cast<u8 const *>(rhs);
    for (usize i = 0; i < n; ++i) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

/// Returns a pointer to the *matching byte*, not to the start of the buffer.
///
/// It used to return `p`, so every hit looked as though it were at offset 0.
/// Nothing in the kernel calls memchr() by name, which is why that survived:
/// the caller that does is libc++, which lowers `std::string_view::find(char)`
/// onto `char_traits<char>::find` and that onto this function. Every such
/// search therefore answered 0 whenever the character occurred anywhere at all
/// -- `"kernel.select.userboot=shell".find('=')` was 0, so
/// obi::CommandLineView::value() split the token at the wrong place and every
/// `key=value` boot option parsed as an empty key with the whole token as its
/// value. A wrong answer rather than a failed one, which is exactly why it
/// stayed hidden: callers test for npos and got a plausible index instead.
///
/// |c| is compared as a byte: both sides go through `char`, and the
/// conversion from int to char keeps exactly the low byte, which is the
/// unsigned-char comparison C asks for (C17 7.24.5.1) whether char is signed
/// or not.
auto memchr(void const *const str, int const c, usize const n) -> void const * {
    auto const *const p = static_cast<char const *>(str);
    for (usize i = 0; i < n; ++i) {
        if (p[i] == static_cast<char>(c)) {
            return p + i;
        }
    }
    return nullptr;
}

auto strlen(char const *str) -> usize {
    usize n = 0;
    while (*str++ != 0) {
        n += 1;
    }
    return n;
}
} // namespace ours::libc
