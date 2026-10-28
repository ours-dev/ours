/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_BOOT_MARKER_HPP
#define ARCH_BOOT_MARKER_HPP 1
// One byte straight to the platform's debug serial port, with no logging
// machinery: boot-step markers have to show even before logging works, or
// when logging is the suspect. Same shape as arch/spin_hint.hpp: each
// arch/<arch>/boot_marker.hpp defines arch::boot_marker() outright.
#if defined(ARCH_X86)
#include <arch/x86/boot_marker.hpp>
#elif defined(ARCH_RISCV)
#include <arch/riscv/boot_marker.hpp>
#endif
#endif // ARCH_BOOT_MARKER_HPP
