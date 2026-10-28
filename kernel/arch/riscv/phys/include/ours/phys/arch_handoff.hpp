/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ARCH_HANDOFF_HPP
#define OURS_PHYS_ARCH_HANDOFF_HPP 1

#include <ours/types.hpp>

namespace ours::phys {
struct Handoff;

/// Escape hatch for whatever one architecture needs that the fields of
/// `Handoff` do not already carry.
struct ArchHandOff {};

/// The final act of the phys world: make the kernel's first instruction
/// executable and jump to it, with the handoff's address in a0. `pgd` goes
/// into satp first, and the instructions that do that have to stay mapped
/// across the switch, since the next fetch is translated by the new root.
///
/// `pgd` has to be the root of phys's own aspace, the only root phys builds
/// and the one that maps the instructions doing the switch. The kernel starts
/// at `entry` with a0 the handoff's physical address, ra and s0 zero, sp
/// still on the boot stack, satp holding `pgd`, and its image already made
/// visible to instruction fetch (fence.i). gp and tp are undefined: the
/// kernel's entry sets both before anything uses them, gp before any code
/// linked with relaxation and tp before any CPU-local access.
///
/// Defined in kernel/arch/riscv/phys/handoff.cpp; never returns.
[[noreturn]]
auto arch_handoff_to_kernel(VirtAddr entry, PhysAddr pgd, Handoff *handoff) -> void;

} // namespace ours::phys

#endif // OURS_PHYS_ARCH_HANDOFF_HPP
