/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_CONFIG_HPP
#define OURS_SYNC_CONFIG_HPP 1

#include <ours/compiler.hpp>

#define OURS_CONFIG_LOCK_TRACING 1

namespace ours::sync {
constexpr static bool const kLockTracingEnabled = OURS_CONFIG_LOCK_TRACING;
} // namespace ours::sync

#endif // #ifndef OURS_SYNC_CONFIG_HPP