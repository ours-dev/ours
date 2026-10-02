/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_STR_HPP
#define OU_STR_HPP 1

#include <ours/types.hpp>

namespace ou {
/// A borrowed, non-owning range of characters.
///
/// The standard library's spelling of this is std::string_view, and this is
/// not a preference for a hand-written substitute: it is what a program with
/// no libc could link. `std::string_view{"text"}` calls strlen() at run time
/// and its comparisons call memcmp(), and a ring-3 program here was linked
/// -nostdlib with no libc behind it, so those were undefined symbols. Every
/// user program now links the functions a freestanding libc++ leans on
/// (//system/lib/libc, through user_executable()), so the remaining step is
/// to make this type an alias of std::string_view; the API stops changing
/// shape then.
///
/// Every entry point that takes text takes one of these rather than a
/// (pointer, length) pair, so that length can never be passed for the wrong
/// pointer. Byte buffers and handle arrays (channel_*, vmo_read, vmo_write)
/// and command_list(char *, usize) are still a pointer and a count, forwarded
/// as given.
class Str {
  public:
    constexpr Str() = default;

    constexpr Str(char const *data, usize size) : m_data(data), m_size(size) {}

    /// Deduces the length of a literal, minus its NUL.
    ///
    /// The NUL is dropped on purpose: every kernel entry point below is given
    /// a length and does not look for a terminator, so sending it would put a
    /// stray byte on the console.
    template <usize N>
    constexpr Str(char const (&literal)[N]) : m_data(literal), m_size(N - 1) {}

    [[nodiscard]]
    constexpr auto data() const -> char const * {
        return m_data;
    }

    [[nodiscard]]
    constexpr auto size() const -> usize {
        return m_size;
    }

    [[nodiscard]]
    constexpr auto empty() const -> bool {
        return m_size == 0;
    }

    /// The prefix of |text| up to its first NUL, or all |limit| characters if
    /// there is none.
    ///
    /// Bounded because the string in question is usually one the kernel wrote
    /// into a buffer the program owns, and a missing terminator would
    /// otherwise run off the end of it. The kernel promises to terminate; this
    /// does not depend on the promise.
    [[nodiscard]]
    static constexpr auto from_c_str(char const *text, usize const limit) -> Str {
        usize size = 0;
        while (size < limit && text[size] != '\0') {
            ++size;
        }
        return {text, size};
    }

  private:
    char const *m_data = nullptr;
    usize m_size = 0;
};
} // namespace ou

#endif // #ifndef OU_STR_HPP
