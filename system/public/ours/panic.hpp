/// SPDX-License-Identifier: MIT
/// Copyright (c) 2025 smallhuazi

#ifndef OURS_PANIC_HPP
#define OURS_PANIC_HPP 1

#include <ours/compiler.hpp>

#include <format>      // std::format_args, std::make_format_args, std::make_wformat_args
#include <string_view> // std::string_view, std::wstring_view
#include <tuple>
#include <type_traits>

namespace ours {
template <typename String, typename FormatArgs>
[[noreturn]]
auto vpanic(String fmt, FormatArgs const &args) -> void;

PHYS_EXPORT [[noreturn]]
auto vpanic(std::string_view fmt) -> void;

/// Bounds-checked stack sink for panic formatting: never allocates, so
/// panic() works in heap-less environments (phys).  Truncation is
/// acceptable for a panic path.
struct PanicSink {
    char *m_out;
    char *m_end;

    struct Iterator {
        using iterator_category = std::output_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = void;

        PanicSink *m_sink;

        auto operator=(char const &value) -> Iterator & {
            if (m_sink->m_out != m_sink->m_end) {
                *m_sink->m_out = value;
                ++m_sink->m_out;
            }
            return *this;
        }

        auto operator*() -> Iterator & {
            return *this;
        }

        auto operator++() -> Iterator & {
            return *this;
        }

        auto operator++(int) -> Iterator {
            return *this;
        }
    };
};

namespace detail {
    /// Hands one panic argument to std::make_format_args in a shape this
    /// build's libc++ actually formats.
    ///
    /// A character *array* -- which is what every string literal is, and what
    /// ASSERT's stringified condition is -- reaches the argument store as an
    /// array type rather than as `const char*`, and comes out of the formatter
    /// as nothing at all: an assertion printed "Assertion failed on " and
    /// stopped, with the condition it had just failed on missing. A pointer or
    /// a string_view formats correctly, so decay the array to a string_view on
    /// the way in. Everything else is forwarded untouched.
    template <typename T>
    FORCE_INLINE constexpr auto as_format_arg(T &&value) -> decltype(auto) {
        using Bare = std::remove_cvref_t<T>;
        if constexpr (std::is_array_v<Bare> && std::is_same_v<std::remove_extent_t<Bare>, char>) {
            return std::string_view(value);
        } else {
            return static_cast<T &&>(value);
        }
    }
} // namespace detail

// Panic formats into a bounded, never-allocating stack buffer (PanicSink)
// in every environment — the phys bootloader included. libc++'s
// std::vformat_to machinery is header-only and freestanding-safe (the
// kernel's logz4 runs the very same code), so there is no reason for a
// boot-time panic to drop its arguments and print bare "{}"s.
template <typename... Args>
FORCE_INLINE [[noreturn]]
auto panic(std::string_view fmt, Args &&...args) -> void {
    if constexpr (sizeof...(args) == 0) {
        vpanic(fmt);
    } else {
        char buffer[512];
        auto sink = PanicSink{buffer, buffer + sizeof(buffer) - 1};
        // The inner lambda is what gives make_format_args() the lvalues it
        // requires: as_format_arg() may hand back a prvalue string_view, and
        // binding that to the lambda's own parameter keeps it alive for the
        // whole formatting call.
        [&](auto &&...prepared) {
            std::vformat_to(PanicSink::Iterator{&sink}, fmt, std::make_format_args(prepared...));
        }(detail::as_format_arg(static_cast<Args &&>(args))...);
        *sink.m_out = '\0';
        vpanic(std::string_view(buffer, static_cast<std::size_t>(sink.m_out - buffer)));
    }
}

FORCE_INLINE [[noreturn]]
inline auto unreachable() -> void {
    panic("Unreachable");
}

FORCE_INLINE [[noreturn]]
inline auto unimplement() -> void {
    panic("Unimplement");
}
} // namespace ours

#endif // #ifndef OURS_PANIC_HPP
