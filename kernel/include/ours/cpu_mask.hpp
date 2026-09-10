/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_CPU_MASK_HPP
#define OURS_CPU_MASK_HPP 1

#include <ours/types.hpp>

#include <ustl/bitset.hpp>

namespace ours {
static_assert(MP_MAX_CPUS > 0, "MP_MAX_CPUS must define a non-empty CPU mask");

inline constexpr auto MAX_CPU = usize{MP_MAX_CPUS};

using CpuMask = ustl::BitSet<MAX_CPU, CpuNum>;
} // namespace ours

#endif // OURS_CPU_MASK_HPP
