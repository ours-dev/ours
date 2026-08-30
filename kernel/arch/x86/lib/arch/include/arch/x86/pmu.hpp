/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_PMU_HPP
#define ARCH_X86_PMU_HPP

#include <arch/types.hpp>
#include <arch/x86/msr.hpp>

namespace arch {

/// Architectural PMU identification, [intel/sdm] 20.2.2: CPUID leaf 0xA.
struct PmuInfo final {
    /// Architectural Performance Monitoring version; 0 means the PMU is
    /// absent (common under QEMU TCG).
    u8 version;
    /// Number of general-purpose counters per logical CPU.
    u8 num_counters;
    /// Counter width in bits; IA32_PMCx accumulate modulo 2^width.
    u8 counter_bits;
};

/// Read CPUID leaf 0xA and interpret the architectural PMU capabilities.
[[nodiscard]]
inline auto detect_pmu() noexcept -> PmuInfo {
    u32 max_leaf = 0;
    u32 ebx = 0;
    u32 ecx = 0;
    u32 edx = 0;
    u32 eax = 0;
    asm volatile("cpuid" : "=a"(max_leaf), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(u32{0}));
    if (max_leaf < 0xA) {
        return PmuInfo{.version = 0, .num_counters = 0, .counter_bits = 0};
    }

    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(u32{0xA}));
    // eax[7:0] version, eax[15:8] number of counters, eax[23:16] bit width.
    return PmuInfo{
        .version = static_cast<u8>(eax & 0xFF),
        .num_counters = static_cast<u8>((eax >> 8) & 0xFF),
        .counter_bits = static_cast<u8>((eax >> 16) & 0xFF),
    };
}

/// Read general-purpose counter `id` through RDPMC.
[[nodiscard]]
inline auto read_pmu_counter(u32 id) noexcept -> u64 {
    u32 low = 0;
    u32 high = 0;
    asm volatile("rdpmc" : "=a"(low), "=d"(high) : "c"(id));
    return (static_cast<u64>(high) << 32) | low;
}

// IA32_PERFEVTSELx fields, [intel/sdm] 20.2.4.
namespace pmu {
    // Event select for "UnHalted Core Cycles" — the canonical cycle-based
    // sampling clock: counts while the logical CPU is not in a halt state.
    constexpr u8 EVENT_UNHALTED_CORE_CYCLES = 0x3C;
    constexpr u8 UMASK_UNHALTED_CORE_CYCLES = 0x00;

    // IA32_PERFEVTSELx control bits.
    constexpr u64 EVTSEL_UMASK_SHIFT = 8;
    constexpr u64 EVTSEL_INT_ENABLE = u64{1} << 20;  // PMI on overflow
    constexpr u64 EVTSEL_ENABLE = u64{1} << 22;      // counting enabled
    constexpr u64 EVTSEL_OS_MODE = u64{1} << 17;     // count at CPL == 0
    constexpr u64 EVTSEL_USER_MODE = u64{1} << 16;   // count at CPL > 0

    // IA32_PERF_GLOBAL_CTRL / _STATUS / _OVF_CTRL bit for general-purpose
    // counter N is bit N (PMC0 = bit 0).
    constexpr u64 GLOBAL_CTRL_PMC0 = u64{1} << 0;
    constexpr u64 GLOBAL_STATUS_OVF_PMC0 = u64{1} << 0;

    // IA32_PERF_GLOBAL_STATUS bit set when an OVF event is pending; also
    // written to IA32_PERF_GLOBAL_OVF_CTRL through bit 62 on PMU v2+.
    constexpr u64 GLOBAL_STATUS_OVF_BUF = u64{1} << 62;
} // namespace pmu

/// Program general-purpose counter 0 to count `event`/`umask` and raise a
/// PMI every `period` counts, then enable it globally. Callers must unmask
/// the APIC LVT PMI entry separately.
inline auto pmu_program_counter0(u8 event, u8 umask, u64 period, bool kernel_mode, bool user_mode)
    -> void {
    u64 const evtsel = (static_cast<u64>(umask) << pmu::EVTSEL_UMASK_SHIFT) |
                       static_cast<u64>(event) |
                       (kernel_mode ? pmu::EVTSEL_OS_MODE : 0) |
                       (user_mode ? pmu::EVTSEL_USER_MODE : 0) | pmu::EVTSEL_INT_ENABLE |
                       pmu::EVTSEL_ENABLE;

    MsrIo::write(MsrRegAddr::IA32PerfEvtSel0, evtsel);
    // Counters count up towards overflow; a reload value of -period makes
    // the interrupt fire every `period` counts.
    MsrIo::write(MsrRegAddr::IA32Pmc0, ~period + 1);
    MsrIo::write(MsrRegAddr::IA32PerfGlobalOvfCtrl, pmu::GLOBAL_STATUS_OVF_PMC0);
    MsrIo::write(MsrRegAddr::IA32PerfGlobalCtrl, pmu::GLOBAL_CTRL_PMC0);
}

/// Disable counter 0 globally and acknowledge any pending overflow.
inline auto pmu_stop_counter0() -> void {
    MsrIo::write(MsrRegAddr::IA32PerfGlobalCtrl, 0);
}

/// Acknowledge a counter-0 overflow and re-arm it with `-period`.
inline auto pmu_rearm_counter0(u64 period) -> void {
    MsrIo::write(MsrRegAddr::IA32PerfGlobalOvfCtrl, pmu::GLOBAL_STATUS_OVF_PMC0);
    MsrIo::write(MsrRegAddr::IA32Pmc0, ~period + 1);
}

} // namespace arch

#endif // ARCH_X86_PMU_HPP
