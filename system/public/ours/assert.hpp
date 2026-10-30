/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_ASSERT_HPP
#define OURS_ASSERT_HPP

#include <ours/panic.hpp>

/// The message, when there is one, is a format string of its own -- it is
/// rendered with its own arguments and appended after the condition. Which of
/// the two panic_assertion() overloads runs is settled by overload
/// resolution on the argument count, not by an `if constexpr` on
/// `__VA_OPT__`: with `if constexpr` in a non-template context both branches
/// still have to compile, so the no-message expansion had to name a call it
/// would never make, and that call quietly went stale.
#define ASSERT(condition, ...)                                                                     \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ours::panic_assertion(#condition __VA_OPT__(, ) __VA_ARGS__);                          \
        }                                                                                          \
    } while (0)

/// Assertions follow KERNEL_DEBUG_LEVEL, the build's one debug switch: any
/// level above 0 turns them on. It is set by //kernel/products:kernel_debug and
/// //build/switch:debug_level_* from the kernel_debug_level toolchain argument,
/// and every kernel and phys switchset defines it.
///
/// This used to test a bare `DEBUG`, which nothing anywhere defines -- so
/// DEBUG_ASSERT expanded to nothing in *every* configuration, debug builds
/// included. That is how `MemCache::allocate`'s
/// `DEBUG_ASSERT(nid < MAX_NODES)` came to sit silently over an
/// out-of-bounds read that hung the boot instead of reporting it.
///
/// `if constexpr` rather than #if for the body: it keeps the condition and its
/// arguments compiled -- and therefore honest -- at debug level 0, so an
/// assertion cannot rot into something that no longer builds once the level is
/// turned up. That costs nothing at runtime; the discarded branch emits no code.
///
/// The outer #if is still needed for translation units that never got the
/// define at all (host tools, tests), where `if constexpr (KERNEL_DEBUG_LEVEL)`
/// would be a compile error rather than a silently-off feature.
#if defined(KERNEL_DEBUG_LEVEL)
#define DEBUG_ASSERT(condition, ...)                                                               \
    do {                                                                                           \
        if constexpr (KERNEL_DEBUG_LEVEL) {                                                        \
            ASSERT(condition, __VA_ARGS__);                                                        \
        }                                                                                          \
    } while (0)
#else
#define DEBUG_ASSERT(condition, ...)
#endif // KERNEL_DEBUG_LEVEL

#endif // OURS_ASSERT_HPP