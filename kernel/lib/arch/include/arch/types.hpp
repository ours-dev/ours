/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_TYPES_HPP
#define ARCH_TYPES_HPP 1

#include <ours/macro_abi.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/types.hpp>

namespace arch {
/// The follwoing types which was defined in subnamespaces of `ours`
/// is immigrating into `arch` namespace grudually.
using ours::HIrqNum;
using ours::PhysAddr;
using ours::VIrqNum;
using ours::VirtAddr;
typedef usize PteVal;

} // namespace arch

#endif // #ifndef ARCH_TYPES_HPP
