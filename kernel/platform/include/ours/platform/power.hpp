/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_POWER_HPP
#define OURS_PLATFORM_POWER_HPP 1

namespace ours {
/// Resets the whole machine, as the reset button would, and never returns.
///
/// A warm reset: the firmware runs again, but RAM keeps its contents, which
/// is what lets a crash record written before it be read after it. Callers
/// that need memory to survive write the cache back first
/// (arch::clean_cache_range()); a reset does not.
[[noreturn]]
auto platform_reboot() -> void;
} // namespace ours

#endif // OURS_PLATFORM_POWER_HPP
