/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_TICK_HPP
#define ARCH_TICK_HPP 1

#include <arch/types.hpp>

namespace arch {

/// A monotonic hardware tick count (e.g. x86's TSC). The counting rate is
/// architecture- (and sometimes platform-) specific; converting to a time
/// unit is left to the caller.
struct Tick final {
    usize tsc;

    [[nodiscard]]
    static auto get() -> Tick;
};

} // namespace arch

// Pulls in the current architecture's definition of Tick::get(). Picking the
// right one this way (rather than making every caller separately include
// e.g. <arch/x86/tick.hpp>) means a single #include <arch/tick.hpp> is
// always enough, with no include-order requirement. See arch/x86/tick.hpp.
#if defined(ARCH_X86)
#include <arch/x86/tick.hpp>
#elif defined(ARCH_ARM)
#include <arch/arm/tick.hpp>
#elif defined(ARCH_RISCV)
#include <arch/riscv/tick.hpp>
#endif

#endif // ARCH_TICK_HPP
