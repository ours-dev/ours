/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SYSCALL_SRC_ARGUMENT_HPP
#define SYSCALL_SRC_ARGUMENT_HPP 1

#include <type_traits>

#include <ours/types.hpp>
#include <user_copy/user_copy.hpp>

namespace ours::syscall {
template <typename T>
struct IsUserPtr: std::false_type {};

template <typename T, user_copy::Direction D>
struct IsUserPtr<user_copy::UserPtr<T, D>>: std::true_type {};

/// A system-call argument, turned from the register it arrived in into the
/// type its handler declares.
///
/// The generated shims (syscall_abi/kernel_shims.inc) call this once per
/// argument, and it is the only conversion they make. Three cases:
///
///   - A pointer becomes a user_copy::UserPtr, here and nowhere else. The
///     register holds a number the user chose, and this is the point where it
///     is given a type that can only be copied through, never dereferenced.
///
///   - An enum -- a handle, an option word, a signal set -- is truncated to
///     its underlying type first and then converted. This is where a number
///     becomes an ours::Uandle on the way in.
///
///   - An integer is truncated to its width.
///
///   Both truncate for one reason. A handle, a count or an option word
///   arrives as a u32 in the low half of a 64-bit register, and SysV leaves
///   the upper half of that register unspecified. It is cut off here rather
///   than trusted to be zero: a caller whose compiler left garbage above a
///   valid handle has still passed a valid handle.
template <typename T>
[[nodiscard]]
constexpr auto from_register(usize const value) -> T {
    if constexpr (IsUserPtr<T>::value) {
        return T(value);
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(value));
    } else {
        static_assert(std::is_integral_v<T>,
                      "a system-call argument is a pointer, an enum or an integer");
        return static_cast<T>(value);
    }
}
} // namespace ours::syscall

#endif // SYSCALL_SRC_ARGUMENT_HPP
