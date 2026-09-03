/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/clock.hpp>
#include <logz4/log.hpp>
#include <ours/start.hpp>

#if defined(ARCH_X86)
#include <arch/interrupt.hpp>
#include <arch/tick.hpp>
#include <arch/x86/io.hpp>
#include <gktl/init_hook.hpp>
#include <ours/mm/cpu_local.hpp>

#include <atomic>

namespace {
constexpr u16 COM1 = 0x3F8;

/// Polls COM1. The lock is a TAS, not ours::sync::SpinLock: bootstrap
/// logging is installed before CpuLocal::init_early(), and SpinLock
/// reads current_cpu() on acquire.
class SerialLogger final: public logz4::Logger {
  public:
    auto write(char const *s, usize n) -> void override {
        auto const irq = arch::local_irq_save_state();
        while (m_lock.test_and_set(std::memory_order_acquire)) {
        }

        auto const is_transmit_empty = [&]() { return arch::inb(COM1 + 5) & 0x20; };
        for (usize i = 0; i < n; ++i) {
            while (!is_transmit_empty()) {
            }
            arch::outb(COM1, static_cast<u8>(s[i]));
        }

        m_lock.clear(std::memory_order_release);
        arch::local_irq_restore_state(irq);
    }

  private:
    std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
};

SerialLogger g_serial;

auto read_time() -> logz4::Time {
    return arch::Tick::get().tsc;
}

auto read_cpu() -> u32 {
    return ours::mm::CpuLocal::current_cpu();
}

auto install_cpu() -> void {
    logz4::set_cpu(read_cpu);
}
} // namespace

auto logz4::emergency_write(char const *s, usize n) -> void {
    g_serial.write(s, n);
}

namespace ours {
auto init_bootstrap_logging() -> void {
    logz4::set_clock(read_time);
    logz4::init(&g_serial);
}
} // namespace ours

GKTL_INIT_HOOK(Logz4Cpu, install_cpu, gktl::InitLevel::CpuLocal);
#else
namespace ours {
auto init_bootstrap_logging() -> void {}
} // namespace ours
#endif
