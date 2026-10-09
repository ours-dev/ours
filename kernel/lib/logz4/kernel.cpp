/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/start.hpp>

#include <logz4/clock.hpp>
#include <logz4/domain.hpp>
#include <logz4/log.hpp>

#if defined(ARCH_X86)
#include <ours/arch/apic.hpp>
#include <ours/cpu.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/cfg.hpp>

#include <atomic>

#include <arch/interrupt.hpp>
#include <arch/spin_hint.hpp>
#include <arch/tick.hpp>
#include <arch/x86/io.hpp>
#include <gktl/init_hook.hpp>

namespace {
constexpr u16 COM1 = 0x3F8;

/// One CPU's account of its console writes, in TSC ticks: how often a write
/// found the lock taken and how long it waited, and how long a write kept
/// interrupts off from first to last.
///
/// It is there to tell where a CPU's interrupts-off time goes. A CPU that
/// keeps interrupts off for longer than one tick period loses a tick (the
/// emulated periodic tick counts a missed period without running the tick),
/// and write() is a stretch with interrupts off: the wait for the lock and
/// then one poll of COM1 per byte.
///
/// Each CPU's account is written only by that CPU, inside write(), with
/// interrupts off, so a field moves on with a plain load and store rather
/// than an atomic read-modify-write. The fields are atomics only so that
/// `lockup status`, on another CPU, may read them while they change.
/// Relaxed, because each is a count read for its own sake: nothing is
/// published through it. The one second writer is an NMI's write nested in
/// this CPU's own, which only panic mode lets through, and the worst it does
/// is lose a count.
class WriteAccount final {
  public:
    struct Totals final {
        u64 writes;
        u64 waits;
        u64 wait_ticks;
        u64 longest_wait;
        u64 longest_off;
        u64 offs_over_tick;
        u64 ticks_past_tick;
    };

    /// A write that found the lock taken and waited |ticks| for it.
    auto add_wait(u64 const ticks) -> void {
        bump(m_waits, 1);
        bump(m_wait_ticks, ticks);
        raise(m_longest_wait, ticks);
    }

    /// A write that kept interrupts off for |ticks|, |tick_period| being one
    /// tick period in TSC ticks, or 0 while the TSC's rate is unknown.
    ///
    /// A stretch longer than a period is also added up by how far it ran
    /// past one period: a stretch of L loses L / period - 1 ticks on
    /// average, however the tick's deadlines fall in it, so that sum over a
    /// period is how many ticks the console cost this CPU.
    auto add_write(u64 const ticks, u64 const tick_period) -> void {
        bump(m_writes, 1);
        raise(m_longest_off, ticks);
        if (tick_period != 0 && ticks > tick_period) {
            bump(m_offs_over_tick, 1);
            bump(m_ticks_past_tick, ticks - tick_period);
        }
    }

    [[nodiscard]]
    auto totals() const -> Totals {
        return {
            .writes = m_writes.load(std::memory_order_relaxed),
            .waits = m_waits.load(std::memory_order_relaxed),
            .wait_ticks = m_wait_ticks.load(std::memory_order_relaxed),
            .longest_wait = m_longest_wait.load(std::memory_order_relaxed),
            .longest_off = m_longest_off.load(std::memory_order_relaxed),
            .offs_over_tick = m_offs_over_tick.load(std::memory_order_relaxed),
            .ticks_past_tick = m_ticks_past_tick.load(std::memory_order_relaxed),
        };
    }

  private:
    static auto bump(std::atomic<u64> &field, u64 const by) -> void {
        field.store(field.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
    }

    static auto raise(std::atomic<u64> &field, u64 const to) -> void {
        if (to > field.load(std::memory_order_relaxed)) {
            field.store(to, std::memory_order_relaxed);
        }
    }

    std::atomic<u64> m_writes{0};
    std::atomic<u64> m_waits{0};
    std::atomic<u64> m_wait_ticks{0};
    std::atomic<u64> m_longest_wait{0};
    std::atomic<u64> m_longest_off{0};
    std::atomic<u64> m_offs_over_tick{0};
    std::atomic<u64> m_ticks_past_tick{0};
};

/// Upper-case like every other CPU_LOCAL in the tree, hence the NOLINT.
CPU_LOCAL
WriteAccount WRITE_ACCOUNT; // NOLINT(readability-identifier-naming)

/// This CPU's account, or null while it has no CPU-local storage to keep one
/// in: before the hook that tells logz4 which CPU writes a line is
/// installed, at InitLevel::CpuLocal. Every CPU that logs after that point
/// has its storage, or logz4's own CPU tag on the line would already read
/// garbage. Before it only the boot CPU runs, so no write waits.
auto this_cpu_account() -> WriteAccount * {
    if (logz4::cpu() == nullptr) {
        return nullptr;
    }
    return &ours::mm::CpuLocal::access(WRITE_ACCOUNT);
}

/// One tick period in TSC ticks, from the TSC's rate as the local APIC's
/// calibration measured it; 0 before the calibration.
auto tick_period() -> u64 {
    return ours::apic_tsc_hz() / static_cast<u64>(ours::time::HZ);
}

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
        auto const off_since = arch::Tick::get().tsc;
        auto *const account = this_cpu_account();
        auto const locked = acquire(account);

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
        if (account != nullptr) {
            account->add_write(arch::Tick::get().tsc - off_since, tick_period());
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
    /// cannot.
    ///
    /// A wait outside panic mode goes into |account|, when there is one; a
    /// lock taken at the first try touches nothing there.
    auto acquire(WriteAccount *const account) -> bool {
        if (!logz4::in_panic_mode()) {
            if (!m_lock.test_and_set(std::memory_order_acquire)) {
                return true;
            }
            auto const start = arch::Tick::get().tsc;
            while (m_lock.test_and_set(std::memory_order_acquire)) {
            }
            if (account != nullptr) {
                account->add_wait(arch::Tick::get().tsc - start);
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
    ///
    /// The logger's one yes/no fact, so it has no word to share; an atomic
    /// because every CPU's writer reads it without holding m_lock, the lock
    /// it gives up on.
    std::atomic<bool> m_abandoned{false};
};

SerialLogger SERIAL;

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
logz4::Domain const *BOOT_DOMAIN = nullptr;

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
    return BOOT_DOMAIN;
}

/// Same level as install_cpu() and for the same reason: both hooks read
/// CPU-local storage, which does not exist before this level.
auto install_domain_slot() -> void {
    logz4::set_domain_slot(domain_slot);
}
} // namespace

auto logz4::emergency_write(char const *s, usize n) -> void {
    SERIAL.write(s, n);
}

namespace ours {
auto init_bootstrap_logging() -> void {
    logz4::set_clock(read_time);
    logz4::init(&SERIAL);
}

auto hold_console_lock_forever() -> void {
    SERIAL.hold_lock_forever();
}

/// Every online CPU's WriteAccount, one line each, in milliseconds to the
/// microsecond, with the ticks its long stretches cost the CPU. Reads each
/// account while its CPU may be adding to it, so a line's numbers may be a
/// write apart from one another.
auto log_console_waits() -> void {
    auto const tsc_hz = apic_tsc_hz();
    auto const period = tick_period();
    if (period == 0) {
        log::info("console: the TSC's rate is not known yet, so no times can be given");
        return;
    }
    // Microseconds: a TSC at 5 GHz for a day is about 4.3e14 ticks, and a
    // million times that still fits in 64 bits.
    auto const us = [tsc_hz](u64 const ticks) -> u64 { return ticks * 1'000'000 / tsc_hz; };
    Cpu::online_mask().for_each([&us, period](CpuNum const cpu) {
        auto const totals = mm::CpuLocal::access(WRITE_ACCOUNT, cpu).totals();
        auto const waited = us(totals.wait_ticks);
        auto const longest_wait = us(totals.longest_wait);
        auto const longest_off = us(totals.longest_off);
        log::info("console: cpu{} wrote {} lines; {} found the lock taken and waited {}.{:03} ms "
                  "in all, the longest {}.{:03} ms; {} kept interrupts off for longer than a "
                  "tick, the longest {}.{:03} ms, which cost about {} ticks",
                  cpu, totals.writes, totals.waits, waited / 1000, waited % 1000,
                  longest_wait / 1000, longest_wait % 1000, totals.offs_over_tick,
                  longest_off / 1000, longest_off % 1000, totals.ticks_past_tick / period);
    });
}
} // namespace ours

GKTL_INIT_HOOK(Logz4Cpu, install_cpu, gktl::InitLevel::CpuLocal);
GKTL_INIT_HOOK(Logz4DomainSlot, install_domain_slot, gktl::InitLevel::CpuLocal);
#else
namespace ours {
auto init_bootstrap_logging() -> void {}
auto hold_console_lock_forever() -> void {}
auto log_console_waits() -> void {}
} // namespace ours
#endif
