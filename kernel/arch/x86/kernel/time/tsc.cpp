/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The TSC as a time source. Enrolled only when CPUID names the TSC
/// invariant (fixed rate, coherent across CPUs) and the APIC calibration
/// measured its frequency: those are what make it safe to sample from
/// arbitrary threads. Rated above the HPET (250), which stays enrolled as
/// the fallback the design anticipates for a machine without an invariant
/// TSC -- the better source switches the clocks live.
///
/// The frequency comes from the APIC calibration rather than a fresh
/// measurement: it already timed the TSC against a clock source over two
/// windows, which is exactly the work a calibration would redo.
#include <ours/arch/apic.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>

#include <arch/tick.hpp>
#include <gktl/canary.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>

namespace ours::time {

/// Above the HPET's 250, below nothing that exists today.
constexpr auto TSC_CLKSRC_RATING = ClockSource::Rating(300);

class TscClockSource {
    using Self = TscClockSource;

  public:
    TscClockSource() : m_canary(this), m_clksrc("TSC-CLKSRC", &VTABLE) {}

    auto read() -> u64 {
        m_canary.verify<Self>();
        return ::arch::Tick::get().tsc;
    }

    auto high_level_source() -> ClockSource & {
        return m_clksrc;
    }

  private:
    gktl::Canary m_canary;
    ClockSource m_clksrc;

    using Packer = ClockSource::Vtable::Packer<&Self::m_clksrc>;
    constexpr static ClockSource::Vtable VTABLE{
        .read = Packer::pack<&Self::read>(),
        .enable = nullptr,
        .disable = nullptr,
        .suspend = nullptr,
        .resume = nullptr,
    };
};

TscClockSource TSC_CLOCK_SOURCE;

} // namespace ours::time

namespace {
auto tsc_time_init() -> void {
    if (!ours::x86_has_feature(arch::CpuFeatureType::InvariantTsc)) {
        log::info("TSC: not invariant, the clock stays on the HPET");
        return;
    }
    auto const hz = ours::apic_tsc_hz();
    if (hz == 0) {
        log::info("TSC: no calibrated frequency, the clock stays on the HPET");
        return;
    }
    auto const _ = ours::time::ClockSource::enroll(
        &ours::time::TSC_CLOCK_SOURCE.high_level_source(), hz, ours::time::TSC_CLKSRC_RATING);
}

GKTL_INIT_HOOK(InitTscClockSource, tsc_time_init, gktl::InitLevel::Irq + 5);
} // namespace
