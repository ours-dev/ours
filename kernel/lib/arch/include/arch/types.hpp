/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_TYPES_HPP
#define ARCH_TYPES_HPP 1

#include <ours/mm/cfg.hpp>
#include <ours/types.hpp>

namespace arch {
/// The follwoing types which was defined in subnamespaces of `ours`
/// is immigrating into `arch` namespace grudually.
using ours::HardwareIrq;
using ours::IrqId;
using ours::PhysAddr;
using ours::VirtAddr;
typedef usize PteVal;

} // namespace arch

#endif // #ifndef ARCH_TYPES_HPP
