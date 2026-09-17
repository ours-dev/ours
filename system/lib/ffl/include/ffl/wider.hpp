/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FFL_WIDER_HPP
#define FFL_WIDER_HPP 1

#include <type_traits>

namespace ffl {

/// The next integer type up, used as the intermediate of a fixed-point
/// multiply or divide.
///
/// Both operations need it for the same reason. Multiplying two Q<i>.<f>
/// values produces a Q<2i>.<2f> product before it is shifted back down, and
/// dividing has to shift the numerator *up* by f before the division to keep
/// the quotient's fraction. Either step overflows the operand type for
/// perfectly ordinary inputs, so the intermediate has to be wider and the
/// result is narrowed once, at the end, with saturation.
///
/// 64-bit widens to `__int128`, which is a compiler extension rather than a
/// standard type. That is acceptable here and nowhere near as exotic as it
/// looks: this tree is clang-only (see //build/toolchain/ours), and both
/// Linux and Zircon rely on the same extension for exactly this -- Linux's
/// mul_u64_u64_shr() and ffl's own 64-bit path.
template <typename T>
struct WiderOf;

template <>
struct WiderOf<i8> {
    using Type = i16;
};
template <>
struct WiderOf<i16> {
    using Type = i32;
};
template <>
struct WiderOf<i32> {
    using Type = i64;
};
template <>
struct WiderOf<i64> {
    using Type = __int128;
};

template <>
struct WiderOf<u8> {
    using Type = u16;
};
template <>
struct WiderOf<u16> {
    using Type = u32;
};
template <>
struct WiderOf<u32> {
    using Type = u64;
};
template <>
struct WiderOf<u64> {
    using Type = unsigned __int128;
};

template <typename T>
using Wider = typename WiderOf<T>::Type;

} // namespace ffl

#endif // #ifndef FFL_WIDER_HPP
