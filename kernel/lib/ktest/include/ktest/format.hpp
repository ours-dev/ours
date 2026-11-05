/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Building the message one failed check leaves behind: an expression, the
// values it compared, and the author's own message, all into a bounded stack
// buffer. Kept free of kernel headers so the host tests exercise it.
#ifndef KTEST_FORMAT_HPP
#define KTEST_FORMAT_HPP 1

#include <format>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ktest::internal {

/// Longest rendering of one compared value; a longer one is cut and marked
/// with `...`.
inline constexpr usize VALUE_ROOM = 64;

/// Longest whole failure message, expression, values and author message
/// included.
inline constexpr usize MESSAGE_ROOM = 256;

/// A write cursor over a bounded stack buffer. The byte at |end| is kept for
/// the terminator finish() writes, so what finish() returns is always a valid
/// C string for the reporter.
struct Buffer {
    char *begin;
    char *out;
    char *end;
};

/// |room| must be at least 1: finish() needs one byte for the terminator, so
/// the buffer holds room - 1 bytes of content.
[[nodiscard]]
constexpr auto make_buffer(char *const storage, usize const room) -> Buffer {
    return Buffer{storage, storage, storage + room - 1};
}

[[nodiscard]]
constexpr auto used(Buffer const &buffer) -> usize {
    return static_cast<usize>(buffer.out - buffer.begin);
}

/// Appends |text|, stopping at the end of the buffer.
constexpr auto append(Buffer &buffer, std::string_view const text) -> void {
    auto const room = static_cast<usize>(buffer.end - buffer.out);
    auto const count = text.size() < room ? text.size() : room;
    for (auto i = usize{0}; i < count; ++i) {
        buffer.out[i] = text[i];
    }
    buffer.out += count;
}

/// Rendering of |value| for a failure line. A type with no `std::formatter`
/// prints as `<unprintable>`, and the check still counts, so no test is
/// blocked on adding one.
template <typename Value>
auto append_value(Buffer &buffer, Value const &value) -> void {
    using Rendered = std::remove_cvref_t<Value>;
    if constexpr (std::formattable<Rendered, char>) {
        auto const available = static_cast<usize>(buffer.end - buffer.out);
        auto room = available < VALUE_ROOM ? available : VALUE_ROOM;
        // Three bytes stay for the ellipsis, so a cut rendering still says
        // that it was cut.
        if (room > 3) {
            room -= 3;
        }
        auto const rendered = std::format_to_n(buffer.out, room, "{}", value);
        auto const size = static_cast<usize>(rendered.size);
        auto const written = size < room ? size : room;
        buffer.out += written;
        if (size > room) {
            append(buffer, "...");
        }
    } else {
        append(buffer, "<unprintable>");
    }
}

/// Formats |format| and its arguments into the rest of the buffer.
template <typename... Args>
auto append_format(Buffer &buffer, std::format_string<Args...> format, Args &&...args) -> void {
    auto const available = static_cast<usize>(buffer.end - buffer.out);
    auto room = available;
    if (room > 3) {
        room -= 3;
    }
    auto const rendered = std::format_to_n(buffer.out, room, format, std::forward<Args>(args)...);
    auto const size = static_cast<usize>(rendered.size);
    auto const written = size < room ? size : room;
    buffer.out += written;
    if (size > room) {
        append(buffer, "...");
    }
}

/// Ends the write and answers what was written, nul-terminated.
[[nodiscard]]
inline auto finish(Buffer &buffer) -> std::string_view {
    *buffer.out = '\0';
    return std::string_view{buffer.begin, used(buffer)};
}

} // namespace ktest::internal

#endif // #ifndef KTEST_FORMAT_HPP
