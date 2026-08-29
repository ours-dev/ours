/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_NAME_HPP
#define KTL_NAME_HPP 1

#include <ktl/types.hpp>

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
    Name(Char const *name, usize len) {
        set(name, len);
    }

    Name(Char const *name) {
        set(name);
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
            return Status::InvalidArguments;
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

} // namespace ktl

#endif // #ifndef KTL_NAME_HPP
