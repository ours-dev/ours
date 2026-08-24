/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_ASSERT_HPP
#define OURS_ASSERT_HPP

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

#ifdef DEBUG
#   define DEBUG_ASSERT(condition, ...) \
        do {\
            if constexpr (DEBUG) {\
                ASSERT(condition, __VA_ARGS__);\
            }\
        } while (0)
#else
#   define DEBUG_ASSERT(condition, ...)
#endif // DEBUG

#endif // OURS_ASSERT_HPP