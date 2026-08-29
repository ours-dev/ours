/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_MACRO_ABI_HPP
#define OURS_MACRO_ABI_HPP 1

// These macros form part of the C++ and assembly ABI.  Keep their expansion
// integer-only so headers shared with early startup assembly remain usable.
#define __OURS_MPL_CONTACT(x, y) x##y
#define _OURS_MPL_CONTACT(x, y) __OURS_MPL_CONTACT(x, y)
#define OURS_MPL_CONTACT(x, y) _OURS_MPL_CONTACT(x, y)

#if defined(__ASSEMBLER__) || defined(__ASSEMBLY__)
#define ULL(x) x
#else
#define ULL(x) x##ULL
#endif

#define KB(x) ((x) * ULL(1024))
#define MB(x) KB((x) * ULL(1024))
#define GB(x) MB((x) * ULL(1024))
#define TB(x) GB((x) * ULL(1024))
#define PB(x) TB((x) * ULL(1024))

#define BIT(pos) (ULL(1) << (pos))

#define BIT_RANGE(start_bit, end_bit)                                                              \
    (((ULL(1) << ((end_bit) - (start_bit) + ULL(1))) - ULL(1)) << (start_bit))

// This must remain a macro because callers use it in template arguments and
// preprocessor-expanded configuration expressions.  The GNU conditional
// operator is supported by the kernel's Clang and its assembler preprocessor.
#define __OURS_BIT_WIDTH_STEP(x, n) ((x) >= (ULL(1) << (n)) ? (n) + 1 : 0)
#define BIT_WIDTH(x)                                                                               \
    (__OURS_BIT_WIDTH_STEP((x), 63) ?: __OURS_BIT_WIDTH_STEP((x), 62) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 61) ?: __OURS_BIT_WIDTH_STEP((x), 60) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 59) ?: __OURS_BIT_WIDTH_STEP((x), 58) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 57) ?: __OURS_BIT_WIDTH_STEP((x), 56) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 55) ?: __OURS_BIT_WIDTH_STEP((x), 54) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 53) ?: __OURS_BIT_WIDTH_STEP((x), 52) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 51) ?: __OURS_BIT_WIDTH_STEP((x), 50) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 49) ?: __OURS_BIT_WIDTH_STEP((x), 48) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 47) ?: __OURS_BIT_WIDTH_STEP((x), 46) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 45) ?: __OURS_BIT_WIDTH_STEP((x), 44) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 43) ?: __OURS_BIT_WIDTH_STEP((x), 42) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 41) ?: __OURS_BIT_WIDTH_STEP((x), 40) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 39) ?: __OURS_BIT_WIDTH_STEP((x), 38) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 37) ?: __OURS_BIT_WIDTH_STEP((x), 36) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 35) ?: __OURS_BIT_WIDTH_STEP((x), 34) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 33) ?: __OURS_BIT_WIDTH_STEP((x), 32) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 31) ?: __OURS_BIT_WIDTH_STEP((x), 30) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 29) ?: __OURS_BIT_WIDTH_STEP((x), 28) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 27) ?: __OURS_BIT_WIDTH_STEP((x), 26) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 25) ?: __OURS_BIT_WIDTH_STEP((x), 24) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 23) ?: __OURS_BIT_WIDTH_STEP((x), 22) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 21) ?: __OURS_BIT_WIDTH_STEP((x), 20) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 19) ?: __OURS_BIT_WIDTH_STEP((x), 18) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 17) ?: __OURS_BIT_WIDTH_STEP((x), 16) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 15) ?: __OURS_BIT_WIDTH_STEP((x), 14) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 13) ?: __OURS_BIT_WIDTH_STEP((x), 12) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 11) ?: __OURS_BIT_WIDTH_STEP((x), 10) ?:          \
   __OURS_BIT_WIDTH_STEP((x), 9) ?: __OURS_BIT_WIDTH_STEP((x), 8) ?:            \
   __OURS_BIT_WIDTH_STEP((x), 7) ?: __OURS_BIT_WIDTH_STEP((x), 6) ?:            \
   __OURS_BIT_WIDTH_STEP((x), 5) ?: __OURS_BIT_WIDTH_STEP((x), 4) ?:            \
   __OURS_BIT_WIDTH_STEP((x), 3) ?: __OURS_BIT_WIDTH_STEP((x), 2) ?:            \
   __OURS_BIT_WIDTH_STEP((x), 1) ?: __OURS_BIT_WIDTH_STEP((x), 0))

#endif // OURS_MACRO_ABI_HPP
