/// SPDX-License-Identifier: MIT
/// Copyright (c) 2025 smallhuazi

#ifndef OURS_PANIC_HPP
#define OURS_PANIC_HPP 1

#include <ours/compiler.hpp>

#include <string_view> // std::string_view, std::wstring_view
#include <format> // std::format_args, std::make_format_args, std::make_wformat_args

namespace ours {
    template <typename String, typename FormatArgs>
    [[noreturn]]
    auto vpanic(String fmt, FormatArgs const &args) -> void;

    template <typename... Args>
    [[noreturn]] FORCE_INLINE
    auto panic(std::string_view fmt, Args &&...args) -> void {
        vpanic(fmt, std::make_format_args(args...));
    }

    template <typename... Args>
    [[noreturn]] FORCE_INLINE
    auto panic(std::wstring_view fmt, Args &&...args) -> void {
        vpanic(fmt, std::make_wformat_args(args...));
    }

    [[noreturn]] FORCE_INLINE
    inline auto unreachable() -> void { panic("Unreachable"); }

    [[noreturn]] FORCE_INLINE
    inline auto unimplement() -> void { panic("Unimplement"); }
} // namespace ours

#endif // #ifndef OURS_PANIC_HPP