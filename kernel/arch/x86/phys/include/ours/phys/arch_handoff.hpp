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
/// executable and jump to it. On x86-64 the kernel's `_start` runs
/// %rip-relative at its linked high virtual addresses and adopts the
/// incoming %cr3 as its own page-table root, so this has to load the
/// kernel's CR3 *before* the jump -- through a trampoline page that is
/// mapped under both the outgoing and the incoming root -- and then jump to
/// `entry` with the physical handoff address in the first-argument register.
///
/// Defined per architecture (x86: kernel/arch/x86/phys/handoff.cpp); never
/// returns.
[[noreturn]]
auto arch_handoff_to_kernel(VirtAddr entry, PhysAddr pgd, Handoff *handoff) -> void;

} // namespace ours::phys

#endif // OURS_PHYS_ARCH_HANDOFF_HPP
