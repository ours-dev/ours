/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_ASSERT_HPP
#define OURS_ASSERT_HPP 1

#include <ours/panic.hpp>

#define ASSERT(condition, ...) \
    do {\
        if (!(condition)) {\
            if constexpr (__VA_OPT__(0 == )1) {\
                ours::panic("Assertion failed on {}", #condition);\
            } else {\
                ours::panic("Assertion failed on {} with message: {}", #condition __VA_OPT__(,) __VA_ARGS__);\
            }\
        }\
    } while (0)

#define DEBUG_ASSERT(condition, ...) \
    do {\
        if constexpr (OURS_DEBUG) {\
            ASSERT(condition, __VA_ARGS__);\
        }\
    } while (0)

#endif // #ifndef OURS_ASSERT_HPP