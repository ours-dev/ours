/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// x86 implementation of the prof::arch seam: programs the architectural PMU
// for cycle-based sampling, routes the overflow through the APIC PMI entry
// and fans start/stop control out to every CPU with IpiService::sync_exec().
#include <ours/arch/apic.hpp>
#include <ours/arch/x86/faults.hpp>
#include <ours/cpu.hpp>
#include <ours/ipi_service.hpp>
#include <ours/irq/mod.hpp>
#include <ours/task/thread.hpp>

#include <optional>

#include <arch/spin_hint.hpp>
#include <arch/tick.hpp>
#include <arch/x86/apic/xapic.hpp>
#include <arch/x86/io.hpp>
#include <arch/x86/msr.hpp>
#include <arch/x86/pmu.hpp>
#include <logz4/log.hpp>
#include <prof/arch.hpp>
#include <prof/sampler.hpp>

namespace {
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
}

constexpr u16 COM1_PORT = 0x3F8;

auto com1_write_byte(u8 byte) -> void {
    while ((::arch::inb(COM1_PORT + 5) & 0x20) == 0) {
    }
    ::arch::outb(COM1_PORT, byte);
}
} // namespace

auto prof::arch::capture_sample_context() -> ustl::Option<prof::SampleContext> {
    auto *frame = ours::irq::current_frame();
    if (frame == nullptr) {
        return ustl::NONE;
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

/// |period| is passed by address into a call that does not return until every
/// CPU has run it, so a local is exactly the right lifetime -- which is the
/// whole reason sync_exec() is synchronous.
auto prof::arch::start_pmu_sampling(std::uint64_t period) -> ours::Status {
    if (period == 0) {
        return ours::Status::BadState;
    }

    ours::IpiService::sync_exec(
        ours::Cpu::online_mask(),
        [](void *context) { program_this_cpu(*static_cast<std::uint64_t *>(context)); }, &period);
    return ours::Status::Ok;
}

/// Returns with every CPU's counter already off, which is what the drain that
/// follows needs: sync_exec() waits for the acknowledgements, so there is
/// nothing left here to count or to time out on.
auto prof::arch::stop_pmu_sampling() -> void {
    ours::IpiService::sync_exec(
        ours::Cpu::online_mask(), [](void *) { teardown_this_cpu(); }, nullptr);
}

auto prof::arch::current_cpu() -> u32 {
    return static_cast<u32>(ours::Cpu::id());
}

auto prof::arch::current_tid() -> u32 {
    return static_cast<u32>(ours::task::Thread::Current::tid());
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
