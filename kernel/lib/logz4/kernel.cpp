/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/start.hpp>

#include <logz4/clock.hpp>
#include <logz4/domain.hpp>
#include <logz4/log.hpp>

#if defined(ARCH_X86)
#include <ours/irq/mod.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/task/thread.hpp>

#include <atomic>

#include <arch/interrupt.hpp>
#include <arch/tick.hpp>
#include <arch/x86/io.hpp>
#include <gktl/init_hook.hpp>

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

/// The log domain of whatever is handling an interrupt on this CPU.
///
/// An interrupt borrows the stack of the thread it lands on but none of its
/// context: a timer tick that arrives while kshell sits in its LogDomain is
/// not kshell speaking. So it neither reads nor writes the thread's slot.
/// One per CPU is enough because an interrupt never leaves its CPU, and a
/// LogDomain opened inside a handler is closed before the handler returns.
///
/// Upper-case like every other CPU_LOCAL in the tree, hence the NOLINT.
CPU_LOCAL
logz4::Domain const *IRQ_DOMAIN; // NOLINT(readability-identifier-naming)

/// Before this CPU has a current thread -- early boot, and an AP between
/// coming up and adopting its idle thread -- there is no per-thread slot to
/// use. Nothing in that window enters a LogDomain today; the slot exists so
/// that doing so is harmless rather than a null dereference.
logz4::Domain const *boot_domain = nullptr;

/// logz4's current-domain slot, per execution context (see
/// logz4::set_domain_slot()): the interrupt's, while one is being handled;
/// otherwise the running thread's.
///
/// "Handling an interrupt" is read off irq::current_frame(), which the IRQ
/// preamble sets and the postamble clears. That is the only such signal the
/// tree keeps: PreemptionState deliberately carries no hard-IRQ count (its
/// Tag says why). The few instructions of entry glue before the preamble run
/// under the thread's slot; they do not log.
auto domain_slot() -> logz4::Domain const *& {
    if (ours::irq::current_frame() != nullptr) {
        return ours::mm::CpuLocal::access(IRQ_DOMAIN);
    }
    if (auto *const thread = ours::task::Thread::Current::get(); thread != nullptr) {
        return thread->log_domain();
    }
    return boot_domain;
}

/// Same level as install_cpu() and for the same reason: both hooks read
/// CPU-local storage, which does not exist before this level.
auto install_domain_slot() -> void {
    logz4::set_domain_slot(domain_slot);
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
GKTL_INIT_HOOK(Logz4DomainSlot, install_domain_slot, gktl::InitLevel::CpuLocal);
#else
namespace ours {
auto init_bootstrap_logging() -> void {}
} // namespace ours
#endif
