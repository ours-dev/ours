/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_NAME_HPP
#define KTL_NAME_HPP 1

#include <ktl/types.hpp>

#include <format>
#include <string_view>

namespace ktl {
// A class for managing names of kernel objects. Since we don't want
// unbounded lengths, the constructor and setter perform
// truncation. Names include the trailing NUL as part of their
// N-sized buffer.
template <usize N, typename CharType = char>
class Name {
    using Self = Name;
    using Char = CharType;
    using CharTraits = std::char_traits<Char>;

  public:
    // Need room for at least one character and a NUL to be useful.
    static_assert(N >= 1u, "Names must have size > 1");

    // Create an empty (i.e., "" with exactly 1 byte: a nul) Name.
    Name() = default;

    // Create a name from the given data. This will be guaranteed to
    // be nul terminated, so the given data may be truncated.
    // A null |name| answers InvArgs from set() and leaves the name empty,
    // which is all a constructor can do with one.
    Name(Char const *name, usize len) {
        auto _ = set(name, len);
    }

    Name(Char const *name) {
        auto _ = set(name);
    }

    ~Name() = default;

    // Copy the Name's data out. The written data is guaranteed to be
    // nul terminated, except when out_len is 0, in which case no data
    // is written.
    auto get(Char *out_name, usize out_len) const -> void {
        if (out_name == nullptr || out_len == 0) {
            return;
        }

        auto copy_len = out_len - 1;
        if (copy_len > N - 1) {
            copy_len = N - 1;
        }
        CharTraits::copy(out_name, name_, copy_len);
        out_name[copy_len] = Char{};
    }

    // Reset the Name to the given data. This will be guaranteed to
    // be nul terminated, so the given data may be truncated.
    auto set(Char const *name, usize len) -> Status {
        if (name == nullptr) {
            name_[0] = Char{};
            return Status::InvArgs;
        }

        if (len > N - 1) {
            len = N - 1;
        }
        CharTraits::copy(name_, name, len);
        name_[len] = Char{};
        return Status::Ok;
    }

    auto set(Char const *name) -> Status {
        if (name == nullptr) {
            return set(name, 0);
        }
        return set(name, CharTraits::length(name));
    }

    auto operator=(Name const &other) -> Self & {
        if (this != &other) {
            CharTraits::copy(name_, other.name_, N);
        }
        return *this;
    }

    auto data() const -> Char const * {
        return name_;
    }

  private:
    // This includes the trailing NULL.
    Char name_[N]{};
};

/// Formats a name into |buffer| and hands it back NUL-terminated.
///
/// The terminator is the whole point. std::format_to neither bounds its writes
/// nor terminates, and a trailing "\0" written into the *format string* does
/// nothing -- a format string is taken as a string_view built with strlen, so
/// the extra NUL is cut off before the formatter ever sees it. A buffer filled
/// that way and passed to Name::set(Char const *) is read by strlen off the end
/// of a stack local; the boot log showed it as thread names like
/// "Dpc/0/Highal", the tail of the previous iteration's "Dpc/0/Normal" showing
/// through.
///
/// format_to_n bounds the write, and its `out` is exactly where the terminator
/// belongs. Every caller that builds a name in a stack buffer should come
/// through here rather than repeating those two facts.
template <usize N, typename... Args>
auto format_name(char (&buffer)[N], std::format_string<Args...> fmt, Args &&...args)
    -> char const * {
    static_assert(N > 1u, "A name buffer must hold at least one character and a NUL");
    auto const end = std::format_to_n(buffer, N - 1, fmt, static_cast<Args &&>(args)...).out;
    *end = '\0';
    return buffer;
}

} // namespace ktl

#endif // #ifndef KTL_NAME_HPP
