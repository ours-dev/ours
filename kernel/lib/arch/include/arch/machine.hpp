/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_MACHINE_HPP
#define ARCH_MACHINE_HPP 1

#include <arch/types.hpp>

namespace arch {

/// What is executing the kernel's instructions.
///
/// A measurement is the reason to ask. A hypervisor runs guest code on the
/// processor itself, so caches and the TLB behave as they do on bare metal,
/// give or take its own page tables. An emulator interprets every
/// instruction and models neither: a timing that depends on a cache or TLB
/// says nothing about a real processor there.
enum class MachineKind {
    /// The processor itself, with nothing underneath.
    BareMetal,
    /// A hypervisor using the processor's virtualization extensions.
    Virtualized,
    /// A program translating or interpreting the instructions.
    Emulated,
};

/// Asks the processor what it is running on. Cheap enough to call when
/// needed, but the answer never changes, so a caller that asks often keeps
/// it.
[[nodiscard]]
auto machine_kind() -> MachineKind;

[[nodiscard]]
constexpr auto to_string(MachineKind kind) -> char const * {
    switch (kind) {
        case MachineKind::BareMetal:
            return "bare metal";
        case MachineKind::Virtualized:
            return "virtualized";
        case MachineKind::Emulated:
            return "emulated";
    }
    return "unknown";
}

} // namespace arch

// See arch/tick.hpp for why the architecture completion is pulled in here
// rather than left for each caller to include separately.
#if defined(ARCH_X86)
#include <arch/x86/machine.hpp>
#elif defined(ARCH_ARM)
#include <arch/arm/machine.hpp>
#elif defined(ARCH_RISCV)
#include <arch/riscv/machine.hpp>
#endif

#endif // ARCH_MACHINE_HPP
