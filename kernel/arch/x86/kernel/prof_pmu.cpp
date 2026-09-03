/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// x86 implementation of the prof::arch seam: programs the architectural PMU
// for cycle-based sampling, routes the overflow through the APIC PMI entry
// and fans start/stop control out to every CPU over the generic IPI.
#include <arch/spin_hint.hpp>
#include <arch/tick.hpp>
#include <arch/x86/apic/xapic.hpp>
#include <arch/x86/io.hpp>
#include <arch/x86/msr.hpp>
#include <arch/x86/pmu.hpp>
#include <logz4/log.hpp>
#include <ours/arch/apic.hpp>
#include <ours/arch/x86/faults.hpp>
#include <ours/cpu.hpp>
#include <ours/irq/mod.hpp>
#include <ours/task/thread.hpp>
#include <ustl/sync/atomic.hpp>

#include <optional>

#include <prof/arch.hpp>
#include <prof/sampler.hpp>

namespace {
enum class IpiAction : std::uint32_t {
    None,
    Start,
    Stop,
};

ustl::sync::Atomic<std::uint32_t> g_ipi_action{static_cast<std::uint32_t>(IpiAction::None)};
ustl::sync::Atomic<std::uint64_t> g_ipi_period{0};
ustl::sync::Atomic<std::uint32_t> g_quiesce_count{0};

auto program_this_cpu(std::uint64_t period) -> void {
    ::arch::pmu_program_counter0(::arch::pmu::EVENT_UNHALTED_CORE_CYCLES,
                                 ::arch::pmu::UMASK_UNHALTED_CORE_CYCLES, period,
                                 /*kernel_mode=*/true, /*user_mode=*/false);
    // LVT PMI entry: vector + unmasked (the mask bit reads as 0 after a
    // plain vector write).
    ours::XAPIC->write_reg(::arch::XApicRegType::LvtPmi,
                             static_cast<std::uint32_t>(::arch::IrqVec::ApicPmi));
}

auto teardown_this_cpu() -> void {
    ::arch::pmu_stop_counter0();
    ours::XAPIC->mask(::arch::XApicRegType::LvtPmi);
    g_quiesce_count.fetch_add(1, ustl::sync::MemoryOrder::AcqRel);
}

constexpr u16 COM1_PORT = 0x3F8;

auto com1_write_byte(u8 byte) -> void {
    while ((::arch::inb(COM1_PORT + 5) & 0x20) == 0) {
    }
    ::arch::outb(COM1_PORT, byte);
}
} // namespace

auto prof::arch::capture_sample_context() -> std::optional<prof::SampleContext> {
    auto *frame = ours::irq::current_frame();
    if (frame == nullptr) {
        return std::nullopt;
    }
    return prof::SampleContext{
        .pc = frame->ip,
        .sp = frame->usp,
        .bp = frame->bp,
        .tid = static_cast<std::uint32_t>(ours::task::Thread::Current::tid()),
        .from_uspace = (frame->cs & 0x3) != 0,
    };
}

auto prof::arch::pmu_present() -> bool {
    // One detection per boot is enough: the PMU is a hardware property.
    static bool const present = ::arch::detect_pmu().version >= 1;
    return present;
}

auto prof::arch::start_pmu_sampling(std::uint64_t period) -> ours::Status {
    if (period == 0) {
        return ours::Status::BadState;
    }

    g_quiesce_count.store(0, ustl::sync::MemoryOrder::Relaxed);
    g_ipi_period.store(period, ustl::sync::MemoryOrder::Relaxed);
    g_ipi_action.store(static_cast<std::uint32_t>(IpiAction::Start),
                       ustl::sync::MemoryOrder::Release);
    ours::apic_send_ipi_mulitcast(ours::Cpu::online_mask(), ::arch::IrqVec::IpiGeneric,
                                  ::arch::ApicDeliveryMode::Fixed);
    return ours::Status::Ok;
}

auto prof::arch::stop_pmu_sampling() -> void {
    g_ipi_action.store(static_cast<std::uint32_t>(IpiAction::Stop),
                       ustl::sync::MemoryOrder::Release);
    ours::apic_send_ipi_mulitcast(ours::Cpu::online_mask(), ::arch::IrqVec::IpiGeneric,
                                  ::arch::ApicDeliveryMode::Fixed);

    // Wait until every CPU confirmed its counter is off, so the drain sees a
    // quiesced buffer set. The multicast is per-CPU unicast, so the pending
    // count must reach the online count.
    auto const expected = ours::Cpu::num_online_cpus();
    for (std::uint32_t spin = 0; spin < 100'000'000; ++spin) {
        if (g_quiesce_count.load(ustl::sync::MemoryOrder::Acquire) >= expected) {
            return;
        }
        ::arch::spin_hint();
    }
    log::warn("prof: {} CPU(s) did not acknowledge the stop", expected - g_quiesce_count.load());
}

auto prof::arch::current_cpu() -> u32 {
    return static_cast<u32>(ours::Cpu::id());
}

auto prof::arch::timestamp() -> u64 {
    return ::arch::Tick::get().tsc;
}

auto prof::arch::debug_write(char const *bytes, usize len) -> void {
    for (usize i = 0; i < len; ++i) {
        com1_write_byte(static_cast<u8>(bytes[i]));
    }
}

namespace ours {

auto x86_handle_prof_ipi() -> void {
    auto const action = static_cast<IpiAction>(g_ipi_action.exchange(
        static_cast<std::uint32_t>(IpiAction::None), ustl::sync::MemoryOrder::Acquire));
    switch (action) {
        case IpiAction::Start: {
            auto const period = g_ipi_period.load(ustl::sync::MemoryOrder::Relaxed);
            program_this_cpu(period);
            break;
        }
        case IpiAction::Stop: {
            teardown_this_cpu();
            break;
        }
        case IpiAction::None:
            break;
    }
}

auto x86_handle_pmi(arch::IrqFrame *frame) -> void {
    // Halt counting while the handler runs so the sampler never samples
    // itself; re-armed below (or left off by the stop path).
    ::arch::pmu_stop_counter0();

    auto &sampler = prof::Sampler::get();
    if (!sampler.is_running()) {
        return;
    }

    prof::SampleContext const context = {
        .pc = frame->ip,
        .sp = frame->usp,
        .bp = frame->bp,
        .tid = static_cast<std::uint32_t>(task::Thread::Current::tid()),
        .from_uspace = (frame->cs & 0x3) != 0,
    };
    sampler.record_sample(context);

    auto const period = sampler.active_period();
    if (period != 0) {
        ::arch::pmu_rearm_counter0(period);
    }
}

} // namespace ours
