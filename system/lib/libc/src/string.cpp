#include <cstring>

#include <ours/compiler.hpp>
#include <ours/types.hpp>

extern "C" WEAK_LINK
auto memcpy(void *d, void const *s, size_t n) -> void * {
    auto md = static_cast<char *>(d);
    auto ms = static_cast<char const *>(s);
    for (size_t i = 0; i < n; ++i) {
        md[i] = ms[i];
    }

    return d;
}

extern "C" WEAK_LINK
auto memmove(void *d, void const *s, size_t n) -> void * {
    auto md = static_cast<char *>(d);
    auto ms = static_cast<char const *>(s);

    if (size_t(ms) > size_t(md)) {
        for (size_t i = 0; i < n; ++i) {
            md[i] = ms[i];
        }
    } else {
        for (size_t i = n; i > 0; --i) {
            md[i - 1] = ms[i - 1];
        }
    }

    return d;
}

extern "C" WEAK_LINK
auto memset(void *s, int c, size_t n) -> void * {
    auto md = static_cast<u8 *>(s);
    for (size_t i = 0; i < n; ++i) {
        md[i] = static_cast<u8>(c);
    }

    return s;
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
///
/// Linux lib/string.c memcmp() and Zircon kernel/lib/libc/string/memcmp.c
/// read through `unsigned char` the same way.
extern "C" WEAK_LINK
auto memcmp(void const *x, void const *y, size_t n) -> int {
    auto p1 = static_cast<u8 const *>(x);
    auto p2 = static_cast<u8 const *>(y);

    for (size_t i = 0; i < n; i++) {
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
/// The return type is `void const *` because that is what this tree's
/// freestanding <string.h> declares; C's own memchr() returns `void *` from a
/// `const void *`, and reconciling the two is a separate change to the shared
/// header, not to this definition.
WEAK_LINK
auto memchr(void const *str, int c, size_t n) -> void const * {
    auto const *p = static_cast<char const *>(str);

    for (size_t i = 0; i < n; i++) {
        if (p[i] == static_cast<char>(c)) {
            return p + i;
        }
    }

    return nullptr;
}

extern "C" WEAK_LINK
auto strlen(char const *p) -> size_t {
    size_t n = 0;
    while (*p++ != 0) {
        n += 1;
    }
    return n;
}
