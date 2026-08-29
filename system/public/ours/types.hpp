/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_TYPES_HPP
#define OURS_TYPES_HPP 1

namespace ours {
using VirtAddr = usize;
using PhysAddr = usize;

using HIrqNum = u32;
using VIrqNum = u32;

using CpuNum = u32;
constexpr auto INVALID_CPU_NUM = static_cast<CpuNum>(-1);

using NnId = u32;

using PgOff = isize;

using Ticks = usize;
using Time = usize;
using Instant = Time;

using TicksSteady = Ticks;
using InstantSteady = Instant;
using DurationTicksSteady = usize;

using TicksSystem = Ticks;
using InstantSystem = Instant;
using DurationTicksSystem = usize;

using KoId = u32;
using Handle = u32;

enum class Status : u32;
enum class Signals : u32;
enum class Rights : u32;

enum class Clock : u32 {
    System,
    Steady,
};

} // namespace ours

#endif // #ifndef OURS_TYPES_HPP
