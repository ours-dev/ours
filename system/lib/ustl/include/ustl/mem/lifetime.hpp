/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_MEM_LIFETIME_HPP
#define USTL_MEM_LIFETIME_HPP 1

#include <new>
#include <type_traits>

#include <cstddef>

namespace ustl::mem {
/// Begins the lifetime of a `T` in storage that already holds its
/// representation, without running a constructor and without changing a byte.
///
/// This is `std::start_lifetime_as` from C++23. libc++ does not ship it yet
/// (checked against the vendored llvm-23), so it lives here until it does --
/// the signature and semantics match, so the eventual switch is a rename.
///
/// It exists for the one situation the language otherwise has no answer for:
/// the kernel is handed a range of physical memory and wants to treat it as an
/// array of descriptors. Casting a pointer at raw storage does *not* create
/// objects -- reading through such a pointer is undefined however well it
/// happens to work -- and placement-new is wrong here because it would
/// overwrite the bytes that are the whole point. `start_lifetime_as` is the
/// operation in between: the object begins to exist, holding whatever was
/// already there.
///
/// The trick is that `memmove` is one of the operations the standard says
/// implicitly creates objects in its destination ([intro.object]). Moving a
/// region onto itself changes nothing and optimises away completely, so this
/// costs no instructions -- it only tells the compiler that an object is now
/// there.
///
/// `T` must be an implicit-lifetime type: trivially destructible, with a
/// trivial or deleted default constructor. That rules out anything whose
/// constructor does real work, which is exactly the point -- a type that needs
/// its constructor run cannot be conjured out of existing bytes.
template <typename T>
[[nodiscard]] [[gnu::always_inline]]
inline auto start_lifetime_as(void *storage) noexcept -> T * {
    static_assert(__builtin_is_implicit_lifetime(T),
                  "start_lifetime_as requires an implicit-lifetime type: a constructor that has "
                  "to run cannot be conjured out of bytes that are already there.");
    return std::launder(static_cast<T *>(__builtin_memmove(storage, storage, sizeof(T))));
}

template <typename T>
[[nodiscard]] [[gnu::always_inline]]
inline auto start_lifetime_as(void const *storage) noexcept -> T const * {
    return start_lifetime_as<T>(const_cast<void *>(storage));
}

/// The array form: begins the lifetime of `count` consecutive `T`s, and of the
/// array object containing them.
template <typename T>
[[nodiscard]] [[gnu::always_inline]]
inline auto start_lifetime_as_array(void *storage, std::size_t count) noexcept -> T * {
    static_assert(__builtin_is_implicit_lifetime(T),
                  "start_lifetime_as_array requires an implicit-lifetime type.");
    return std::launder(static_cast<T *>(__builtin_memmove(storage, storage, sizeof(T) * count)));
}

template <typename T>
[[nodiscard]] [[gnu::always_inline]]
inline auto start_lifetime_as_array(void const *storage, std::size_t count) noexcept -> T const * {
    return start_lifetime_as_array<T>(const_cast<void *>(storage), count);
}

} // namespace ustl::mem

#endif // USTL_MEM_LIFETIME_HPP
