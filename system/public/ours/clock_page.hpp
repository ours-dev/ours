/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_CLOCK_PAGE_HPP
#define OURS_CLOCK_PAGE_HPP 1

#include <ours/types.hpp>

namespace ours {
/// Process root VMA base, matching the kernel's USER_ASPACE_BASE.
inline constexpr auto PROCESS_ASPACE_BASE = u64{0x200000};
/// The clock MMIO page in every process, one page below the kernel's vDSO
/// base (`PROCESS_ASPACE_BASE + 8 MiB`). The published counter address is
/// this plus |CLOCK_COUNTER_OFFSET|.
inline constexpr auto CLOCK_PAGE_BASE = PROCESS_ASPACE_BASE + (u64{8} << 20) - u64{4096};
inline constexpr auto CLOCK_COUNTER_OFFSET = u64{0xf0};
} // namespace ours

#endif // #ifndef OURS_CLOCK_PAGE_HPP
