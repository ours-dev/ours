/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USER_COPY_COPY_ALLOWED_HPP
#define USER_COPY_COPY_ALLOWED_HPP 1

#include <type_traits>

namespace user_copy {
/// Whether a T may cross the user/kernel boundary as bytes: what UserPtr
/// requires of its element type.
///
///   - Trivially copyable: copying the bytes is a valid copy of the object,
///     with no constructor or destructor to run on either side.
///   - Standard layout: the fields sit where a C declaration of the same
///     struct, compiled into the program, expects them.
///   - Unique object representations: equal values have equal bytes, which
///     rules out padding. A struct with a hole between two fields is copied
///     out hole and all, and the hole holds whatever that kernel stack held
///     before -- a kernel pointer, another process's data. The fix is to
///     name the hole as a field and set it (Zircon's info structs spell it
///     `uint8_t padding1[4]`).
///
/// Zircon's is_copy_allowed (lib/user_copy/internal.h) asks for is_trivial
/// rather than trivially copyable. That also demands a trivial default
/// constructor, which says nothing about whether bytes may be copied, and
/// std::is_trivial is deprecated in C++26, which this tree compiles as.
///
/// A header of its own, depending on <type_traits> only, so that the host
/// tests can check it without the kernel's arch headers that user_copy.hpp
/// needs.
template <typename T>
concept CopyAllowed = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> &&
                      std::has_unique_object_representations_v<T>;

} // namespace user_copy

#endif // #ifndef USER_COPY_COPY_ALLOWED_HPP
