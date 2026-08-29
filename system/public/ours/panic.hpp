/// SPDX-License-Identifier: MIT
/// Copyright (c) 2025 smallhuazi

#ifndef OURS_PANIC_HPP
#define OURS_PANIC_HPP 1

#include <ours/compiler.hpp>

#include <format>      // std::format_args, std::make_format_args, std::make_wformat_args
#include <string_view> // std::string_view, std::wstring_view

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

#if defined(PHYS_ENVIRONMENT)
// The phys image has no formatting runtime (see ours/phys/print.hpp):
// the unformatted text is the best available diagnostic before memory
// services exist, so the arguments are dropped rather than dragging
// std::format's allocating machinery into the bootloader.
template <typename... Args>
FORCE_INLINE [[noreturn]]
auto panic(std::string_view fmt, Args &&...) -> void {
    vpanic(fmt);
}
#else
template <typename... Args>
FORCE_INLINE [[noreturn]]
auto panic(std::string_view fmt, Args &&...args) -> void {
    if constexpr (sizeof...(args) == 0) {
        vpanic(fmt);
    } else {
        char buffer[512];
        auto sink = PanicSink{buffer, buffer + sizeof(buffer) - 1};
        std::vformat_to(PanicSink::Iterator{&sink}, fmt, std::make_format_args(args...));
        *sink.m_out = '\0';
        vpanic(std::string_view(buffer, static_cast<std::size_t>(sink.m_out - buffer)));
    }
}
#endif

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
