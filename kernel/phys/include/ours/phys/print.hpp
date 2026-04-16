/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_PRINT_HPP
#define OURS_PHYS_PRINT_HPP 1

#include <ours/types.hpp>
#include <ours/compiler.hpp>
#include <ustl/fmt/make_format_args.hpp>
#include <ustl/views/string_view.hpp>
#include <ustl/source_location.hpp>

#define dprint(fmt, ...) \
do { \
    if (OURS_DEBUG) {\
        ours::phys::print("[debug|{}|{}] ", __FILE_NAME__, __LINE__);\
        ours::phys::print(fmt, __VA_ARGS__);\
    }\
} while(0)

#define dprintln(fmt, ...) \
do { \
    if (OURS_DEBUG) {\
        ours::phys::print("[debug|{}|{}] ", __FILE_NAME__, __LINE__);\
        ours::phys::println(fmt, __VA_ARGS__);\
    }\
} while(0)

namespace ours::phys {
    auto vprint(ustl::views::StringView fmt) -> void;
    auto vprint(ustl::views::StringView fmt, ustl::fmt::FormatArgs const &args) -> void;

    template <typename... Args> 
    auto print(ustl::views::StringView fmt, Args &&...args) -> void
    { 
        if CXX17_CONSTEXPR (sizeof...(args)) {
            return vprint(fmt, ustl::fmt::make_format_args(args...)); 
        }
        vprint(fmt);
    }

    template <typename... Args> 
    auto println(ustl::views::StringView fmt, Args &&...args) -> void
    { 
        if CXX17_CONSTEXPR (sizeof...(args)) {
            vprint(fmt, ustl::fmt::make_format_args(args...)); 
        } else {
            vprint(fmt);
        }
        vprint("\n");
    }

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_LOG_HPP