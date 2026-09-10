/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_RING_HPP
#define LOGZ4_RING_HPP 1

#include <ours/types.hpp>

#include <logz4/clock.hpp>
#include <logz4/level.hpp>
#include <logz4/logger.hpp>

namespace logz4::ring {

/// Budget of one fully assembled line, prefix and newline included. Lines
/// longer than this are cut and end with an ellipsis.
inline constexpr usize LINE_BYTES = 512;

/// Record slots. 256 lines of history (~136 KiB of .bss): enough to hold
/// a boot's worth of context around whatever ends in a panic. Compile-time
/// constant; grow it by editing here, not at run time.
inline constexpr usize SLOT_COUNT = 256;

/// Append one already-assembled line as the newest record. Safe from any
/// context that can execute an atomic fetch-add and a short memcpy --
/// including multiple CPUs and interrupt handlers concurrently. When the
/// ring is full the oldest record is silently overwritten.
auto append(char const *line, usize len, Time time, u32 cpu, Level level) -> void;

/// Replay the surviving records, oldest first, each as one write() to
/// `sink`. Records are stored pre-rendered, so a panic-time dump performs
/// no formatting work at all. Returns how many records were replayed.
auto dump_to(Logger &sink) -> usize;

/// Wipe every record and restart ticketing. Test scaffolding only.
auto reset_for_test() -> void;

} // namespace logz4::ring

#endif // LOGZ4_RING_HPP
