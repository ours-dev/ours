/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/arch/apic.hpp>
#include <ours/platform/timer.hpp>

namespace ours {
/// On a PC, arch::Tick reads the TSC, and the kernel takes the TSC's rate
/// from the local APIC timer's calibration against the clock source
/// (apic_calibrate_clock()): 0 until the boot CPU has calibrated, at
/// InitLevel::Irq + 4, before any other CPU starts, and 0 for good if the
/// calibration failed. A load of a variable the calibration writes once.
auto arch_tick_hz() -> u64 {
    return apic_tsc_hz();
}
} // namespace ours
