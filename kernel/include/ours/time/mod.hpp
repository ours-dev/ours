/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_MOD_HPP
#define OURS_TIME_MOD_HPP 1

#include <ours/syscall/time.hpp>

namespace ours::time {
    auto current_mono_time() -> Time;

    auto calculate_mult_shift(usize *mult, usize *shift, usize from, usize to, usize maxsec) -> void;

} // namespace ours::time

#endif // #ifndef OURS_TIME_MOD_HPP