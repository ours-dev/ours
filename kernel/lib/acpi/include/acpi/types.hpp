/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_TYPES_HPP
#define ACPI_TYPES_HPP 1

#include <ours/compiler.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace acpi {
// The integer aliases (u8 ... usize) already exist at global scope; they come
// from ours/prelude.hpp, which every toolchain in this tree force-includes --
// including the host toolchain (see //kernel/products:host-prelude, wired up in
// build/config/BUILDCONFIG.gn).  Re-declaring them here, as this header used to,
// only created a second spelling of the same types.
using ours::PhysAddr;
using ours::Status;
using ours::VirtAddr;

} // namespace acpi

#endif // #ifndef ACPI_TYPES_HPP
