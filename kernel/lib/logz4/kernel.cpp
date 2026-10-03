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
#include <arch/spin_hint.hpp>
#include <arch/tick.hpp>
#include <arch/x86/io.hpp>
#include <gktl/init_hook.hpp>

namespace {
constexpr u16 COM1 = 0x3F8;

/// How long a write in panic mode waits for the lock before it writes
/// without it, in TSC ticks.
///
/// Long enough for a live holder to finish its line: 512 bytes at 115200
/// baud take about 45 ms, and this is 50 ms at a 5 GHz TSC and 250 ms at a
/// 1 GHz one. It is paid at most once per panic; see
/// SerialLogger::m_abandoned.
///
/// Ticks, not spins. A spin's cost varies by more than a hundred times:
/// PAUSE takes about ten cycles on some cores and about 140 on others, and
/// under QEMU's TCG one spin of this loop measured about 640 ns -- a bound
/// of ten million spins held the rel image's panic up for 6.4 s.
constexpr auto PANIC_LOCK_TICKS = usize{250'000'000};

/// Polls COM1. The lock is a TAS, not ours::sync::SpinLock: bootstrap
/// logging is installed before CpuLocal::init_early(), and SpinLock
/// reads current_cpu() on acquire.
///
/// The lock keeps one line's bytes together on the wire. Without it, two
/// CPUs each poll the transmitter, each see it empty, and their bytes
/// alternate.
class SerialLogger final: public logz4::Logger {
  public:
    auto write(char const *s, usize n) -> void override {
        auto const irq = arch::local_irq_save_state();
        auto const locked = acquire();

        auto const is_transmit_empty = [&]() { return arch::inb(COM1 + 5) & 0x20; };
        for (usize i = 0; i < n; ++i) {
            while (!is_transmit_empty()) {
            }
            arch::outb(COM1, static_cast<u8>(s[i]));
        }

        // Only a write that took the lock gives it back. Clearing a lock
        // held by someone else would let a third CPU in while the holder is
        // still writing, and the holder's own clear would later let in a
        // fourth: mutual exclusion would be gone for everyone, not just for
        // this one write. A write that went ahead without the lock leaves it
        // exactly as it found it.
        if (locked) {
            m_lock.clear(std::memory_order_release);
        }
        arch::local_irq_restore_state(irq);
    }

    /// Takes the lock the way write() holds it, interrupts off and then the
    /// lock, and gives neither back. For `crash panic-locked` only.
    auto hold_lock_forever() -> void {
        arch::local_irq_disable();
        while (m_lock.test_and_set(std::memory_order_acquire)) {
        }
    }

  private:
    /// Takes the lock and says whether it did.
    ///
    /// Outside panic mode it waits for as long as it takes, and always does.
    ///
    /// In panic mode it waits PANIC_LOCK_TICKS at most, and then gives up,
    /// so that write() goes ahead without the lock. A holder that is alive
    /// finishes its line well inside that bound, so lines stay whole. A
    /// holder that never lets go -- this CPU, if it panicked inside write();
    /// another CPU that stopped answering with the lock held -- costs one
    /// bound, after which the panic prints, interleaved with whatever the
    /// holder may still write. Interleaved output can be read; no output
    /// cannot. Linux's serial8250_console_write() only try-locks the port
    /// while oops_in_progress is set, for the same reason.
    auto acquire() -> bool {
        if (!logz4::in_panic_mode()) {
            while (m_lock.test_and_set(std::memory_order_acquire)) {
            }
            return true;
        }
        if (m_abandoned.load(std::memory_order_relaxed)) {
            return !m_lock.test_and_set(std::memory_order_acquire);
        }

        auto const start = arch::Tick::get().tsc;
        while (m_lock.test_and_set(std::memory_order_acquire)) {
            if (arch::Tick::get().tsc - start >= PANIC_LOCK_TICKS) {
                m_abandoned.store(true, std::memory_order_relaxed);
                return false;
            }
            arch::spin_hint();
        }
        return true;
    }

    std::atomic_flag m_lock = ATOMIC_FLAG_INIT;

    /// Set once a write in panic mode has waited out the whole bound. A
    /// holder that outlasted it is taken to be gone for good, so every later
    /// write tries the lock once and does not wait again. Without this a
    /// panic on a held lock would wait the bound for every one of the 256
    /// lines the replay prints.
    std::atomic<bool> m_abandoned{false};
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

auto hold_console_lock_forever() -> void {
    g_serial.hold_lock_forever();
}
} // namespace ours

GKTL_INIT_HOOK(Logz4Cpu, install_cpu, gktl::InitLevel::CpuLocal);
GKTL_INIT_HOOK(Logz4DomainSlot, install_domain_slot, gktl::InitLevel::CpuLocal);
#else
namespace ours {
auto init_bootstrap_logging() -> void {}
auto hold_console_lock_forever() -> void {}
} // namespace ours
#endif
