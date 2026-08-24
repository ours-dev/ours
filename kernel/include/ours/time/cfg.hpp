/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_CFG_HPP
#define OURS_TIME_CFG_HPP 1

#ifndef OURS_CONFIG_HZ
#define HZ 100
#else
#define HZ OURS_CONFIG_HZ
#endif // #ifndef OURS_CONFIG_HZ

#include <ours/syscall/time.hpp>
#include <ours/types.hpp>

namespace ours::time {
constexpr static auto kNsecPerTick = (duration_cast<Nanoseconds>(Seconds(1)) / HZ);

} // namespace ours::time

#endif // #ifndef OURS_TIME_CFG_HPP