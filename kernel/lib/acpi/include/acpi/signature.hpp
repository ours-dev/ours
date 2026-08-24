/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_SIGNATURE_HPP
#define ACPI_SIGNATURE_HPP 1

#include <acpi/types.hpp>
#include <ustl/views/string_view.hpp>

namespace acpi {
/// A four character ACPI table signature.
///
/// ACPI stores signatures as four ASCII bytes in table order, so the byte at
/// offset 0 is the first character.  The packed little-endian `u32` is the only
/// member, and it is built arithmetically: the previous spelling was a union
/// with a `char[4]` arm, which made every comparison read an inactive member --
/// ill-formed inside a constant expression, and merely tolerated outside one.
///
/// Kept as an aggregate with no user-provided constructors so that it stays
/// POD.  A non-POD member of a PACKED table structure is one clang declines to
/// pack, and says so under -Wpacked-non-pod.
struct Signature {
    /// Binds to the array rather than to a decayed pointer, so a signature
    /// spelled with too few characters is a compile error instead of a read
    /// past the end of the literal.
    [[nodiscard]]
    constexpr static auto from_chars(char const (&name)[5]) -> Signature {
        return Signature{u32(u8(name[0])) | (u32(u8(name[1])) << 8) | (u32(u8(name[2])) << 16) |
                         (u32(u8(name[3])) << 24)};
    }

    [[nodiscard]]
    constexpr static auto from_raw(u32 raw) -> Signature {
        return Signature{raw};
    }

    /// The four characters, in table order.  Not NUL terminated: ACPI pads
    /// short signatures with spaces rather than terminating them.  Reading an
    /// object's representation through `char` is explicitly allowed to alias.
    [[nodiscard]]
    auto name() const -> ustl::views::StringView {
        return {reinterpret_cast<char const *>(&value), 4};
    }

    [[nodiscard]]
    constexpr friend auto operator==(Signature const &x, Signature const &y) -> bool {
        return x.value == y.value;
    }

    u32 value;
};
static_assert(sizeof(Signature) == 4);
static_assert(alignof(Signature) == alignof(u32));

} // namespace acpi

#endif // #ifndef ACPI_SIGNATURE_HPP
