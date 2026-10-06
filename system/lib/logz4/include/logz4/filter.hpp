/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_FILTER_HPP
#define LOGZ4_FILTER_HPP 1

#include <logz4/level.hpp>

#include <atomic>

namespace logz4 {

/// The compile-time noise floor. Images built with the debug level turned
/// up (KERNEL_DEBUG_LEVEL above 0, set by every kernel and phys switchset)
/// start at Trace; release-grade images start at Info. The debug()/trace()
/// call sites are additionally compiled out entirely in release images (see
/// LOGZ4_VERBOSE_LOGGING in log.hpp) so hot paths pay nothing at all.
constexpr Level MIN_COMPILE_LEVEL =
#if defined(KERNEL_DEBUG_LEVEL) && KERNEL_DEBUG_LEVEL
    Level::Trace;
#else
    Level::Info;
#endif

/// The runtime floor. Starts at the compile-time floor; set_min_level()
/// moves it at run time (boot option, console command). A relaxed load and
/// store suffice: the gate is advisory, not a lock.
inline auto runtime_min_level() -> std::atomic<Level> & {
    static std::atomic<Level> LEVEL{MIN_COMPILE_LEVEL};
    return LEVEL;
}

inline auto set_min_level(Level level) -> void {
    runtime_min_level().store(level, std::memory_order_relaxed);
}

[[nodiscard]]
inline auto min_level() -> Level {
    return runtime_min_level().load(std::memory_order_relaxed);
}

[[nodiscard]]
inline auto is_enabled(Level level) -> bool {
    return level >= min_level();
}

} // namespace logz4

#endif // LOGZ4_FILTER_HPP
