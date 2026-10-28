/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTSHIM_CONVERSION_HPP
#define BOOTSHIM_CONVERSION_HPP 1

#include <ours/types.hpp>

namespace bootshim {

/// What a translator's convert() managed to do with a platform's memory map.
///
/// `dropped` is non-zero only when the platform described more ranges than the
/// translator has room for. It is separated from `converted` rather than folded
/// into a bool because it is the number the shim should name when it warns:
/// "converted 64 of 71" tells an operator how much of their machine's memory
/// map went missing, "truncated" does not. What a drop costs depends on the
/// map, so each translator says what its caller must do about one.
struct Conversion final {
    usize converted;
    usize dropped;
};

} // namespace bootshim

#endif // BOOTSHIM_CONVERSION_HPP
