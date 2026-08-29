/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_PRINT_HPP
#define OURS_PHYS_PRINT_HPP 1

#include <ours/compiler.hpp>
#include <ours/types.hpp>
#include <ustl/views/string_view.hpp>

/// Debug-print gate for the phys environment: driven by the debug level the
/// phys toolchains define (kernel/phys/BUILD.gn sets KERNEL_DEBUG_LEVEL).
#ifndef OURS_DEBUG
#if defined(KERNEL_DEBUG_LEVEL)
#define OURS_DEBUG (KERNEL_DEBUG_LEVEL > 0)
#else
#define OURS_DEBUG 0
#endif
#endif

#if defined(PHYS_ENVIRONMENT)
#define dprint(fmt, ...)                                                                           \
    do {                                                                                           \
        if (OURS_DEBUG) {                                                                          \
            ours::phys::print(fmt);                                                                \
        }                                                                                          \
    } while (0)

#define dprintln(fmt, ...)                                                                         \
    do {                                                                                           \
        if (OURS_DEBUG) {                                                                          \
            ours::phys::println(fmt);                                                              \
        }                                                                                          \
    } while (0)
#else
#include <ustl/fmt/make_format_args.hpp>
#include <ustl/source_location.hpp>

#define dprint(fmt, ...)                                                                           \
    do {                                                                                           \
        if (OURS_DEBUG) {                                                                          \
            ours::phys::print("[debug|{}|{}] ", __FILE_NAME__, __LINE__);                          \
            ours::phys::print(fmt, __VA_ARGS__);                                                   \
        }                                                                                          \
    } while (0)

#define dprintln(fmt, ...)                                                                         \
    do {                                                                                           \
        if (OURS_DEBUG) {                                                                          \
            ours::phys::print("[debug|{}|{}] ", __FILE_NAME__, __LINE__);                          \
            ours::phys::println(fmt, __VA_ARGS__);                                                 \
        }                                                                                          \
    } while (0)
#endif

namespace ours::phys {
PHYS_EXPORT auto vprint(ustl::views::StringView fmt) -> void;
#if !defined(PHYS_ENVIRONMENT)
PHYS_EXPORT auto vprint(ustl::views::StringView fmt, ustl::fmt::FormatArgs const &args) -> void;
#endif

template <typename... Args>
auto print(ustl::views::StringView fmt, Args &&...args) -> void {
#if defined(PHYS_ENVIRONMENT)
    vprint(fmt);
#else
    if constexpr (sizeof...(args)) {
        return vprint(fmt, ustl::fmt::make_format_args(args...));
    }
    vprint(fmt);
#endif
}

template <typename... Args>
auto println(ustl::views::StringView fmt, Args &&...args) -> void {
#if defined(PHYS_ENVIRONMENT)
    vprint(fmt);
#else
    if constexpr (sizeof...(args)) {
        vprint(fmt, ustl::fmt::make_format_args(args...));
    } else {
        vprint(fmt);
    }
#endif
    vprint("\n");
}

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_LOG_HPP
