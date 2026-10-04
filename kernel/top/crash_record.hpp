/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TOP_CRASH_RECORD_HPP
#define OURS_TOP_CRASH_RECORD_HPP 1

#include <ours/types.hpp>

#include <string_view>

namespace ours {
/// Writes the panic's message, then the newest `tail_lines` lines of the log
/// ring, into the crash record region (bootmem::MemoryType::Crashlog), seals
/// the record and writes it back from the cache, so that the next boot can
/// read it. Does nothing when the image declared no region, or the panic came
/// before the region was found.
///
/// Called only by the CPU that won vpanic()'s claim_report(), with interrupts
/// off and every other CPU stopped: the region has one writer and needs no
/// lock.
auto record_panic(std::string_view message, usize tail_lines) -> void;
} // namespace ours

#endif // OURS_TOP_CRASH_RECORD_HPP
