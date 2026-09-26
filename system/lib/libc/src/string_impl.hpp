/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LIBC_SRC_STRING_IMPL_HPP
#define LIBC_SRC_STRING_IMPL_HPP 1

/// The implementations behind string.cpp's C names, under names of their own.
///
/// They are here, rather than being the bodies of memcpy() and friends
/// directly, so that a test can call them. A host test cannot call "our
/// memcpy" by that name: the compiler knows what memcpy means and is free to
/// answer a call to it without making one -- a deliberately wrong memcmp
/// linked into a host program still printed the right answer, because clang
/// had evaluated the call itself -- and a program with two memcpy
/// definitions keeps only one of them. ours::libc::memcpy is a name no
/// compiler knows, so a call to it is a call, and it collides with nothing.
///
/// Everything else goes through the C names (string.cpp): these are an
/// implementation detail of the library, not a second API.
namespace ours::libc {
auto memcpy(void *dst, void const *src, usize n) -> void *;

auto memmove(void *dst, void const *src, usize n) -> void *;

auto memset(void *dst, int c, usize n) -> void *;

auto memcmp(void const *lhs, void const *rhs, usize n) -> int;

auto memchr(void const *str, int c, usize n) -> void const *;

auto strlen(char const *str) -> usize;
} // namespace ours::libc

#endif // #ifndef LIBC_SRC_STRING_IMPL_HPP
