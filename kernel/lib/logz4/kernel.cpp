/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/cpu.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/platform/debug_console.hpp>
#include <ours/start.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/ticks_per_second.hpp>
#include <ours/time/types.hpp>

#include <atomic>

#include <arch/interrupt.hpp>
#include <arch/spin_hint.hpp>
#include <arch/tick.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/clock.hpp>
#include <logz4/domain.hpp>
#include <logz4/log.hpp>

namespace {
/// One CPU's account of its console writes, in arch::Tick counts: how often
/// a write found the lock taken and how long it waited, and how long a write
/// kept interrupts off from first to last.
///
/// It is there to tell where a CPU's interrupts-off time goes. A CPU that
/// keeps interrupts off for longer than one tick period loses a tick (the
/// emulated periodic tick counts a missed period without running the tick),
/// and write() is a stretch with interrupts off: the wait for the lock and
/// then the console's own wait on its device for every byte.
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
    /// tick period in arch::Tick counts, or 0 while their rate is unknown.
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

/// One tick period in arch::Tick counts, from the counter's rate; 0 while the
/// rate is not known.
auto tick_period() -> u64 {
    return ours::time::ticks_per_second() / static_cast<u64>(ours::time::HZ);
}

/// How long a write in panic mode waits for the lock before it writes
/// without it.
///
/// Long enough for a live holder to finish its line: 512 bytes at 115200
/// baud take about 45 ms. It is paid at most once per panic; see
/// SerialLogger::m_abandoned.
///
/// A time, not spins. A spin's cost varies by more than a hundred times:
/// PAUSE takes about ten cycles on some cores and about 140 on others, and
/// under QEMU's TCG one spin of this loop measured about 640 ns -- a bound
/// of ten million spins held the rel image's panic up for 6.4 s. And a time,
/// not a count of arch::Tick: the counter runs at 1 GHz under QEMU's x86, at
/// a few GHz on a real one, and at 10 MHz on QEMU's RISC-V board, so one
/// count would be a different wait on each.
constexpr auto PANIC_LOCK_WAIT = ours::Milliseconds(250);

/// PANIC_LOCK_WAIT in arch::Tick counts, or 0 while the counter's rate is
/// not known.
///
/// 0, because then no wait can help. Until the rate is known only the boot
/// CPU runs (ours/time/ticks_per_second.hpp), so a lock a write finds held
/// is this CPU's own: it panicked inside write(), or an NMI interrupted one.
/// That holder never lets go, so acquire() tries once and gives up. The one
/// exception is a PC whose calibration failed, which runs every CPU with the
/// rate unknown: there a panic does not wait for another CPU's line either,
/// and its first line may come out interleaved with that one.
auto panic_lock_ticks() -> u64 {
    return ours::time::ticks_per_second() / 1000 * static_cast<u64>(PANIC_LOCK_WAIT.count());
}

/// Writes through the platform's debug console. The lock is a TAS, not
/// ours::sync::SpinLock: bootstrap logging is installed before
/// CpuLocal::init_early(), and SpinLock reads current_cpu() on acquire.
///
/// The lock keeps one line's bytes together on the wire. Without it, two
/// CPUs each poll the transmitter, each see it empty, and their bytes
/// alternate.
class SerialLogger final: public logz4::Logger {
  public:
    auto write(char const *s, usize n) -> void override {
        auto const irq = arch::local_irq_save_state();
        auto const off_since = arch::Tick::get().count;
        auto *const account = this_cpu_account();
        auto const locked = acquire(account);

        ours::platform::debug_console_write(s, n);

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
            account->add_write(arch::Tick::get().count - off_since, tick_period());
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
    /// In panic mode it waits PANIC_LOCK_WAIT at most, and then gives up,
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
            auto const start = arch::Tick::get().count;
            while (m_lock.test_and_set(std::memory_order_acquire)) {
            }
            if (account != nullptr) {
                account->add_wait(arch::Tick::get().count - start);
            }
            return true;
        }
        if (m_abandoned.load(std::memory_order_relaxed)) {
            return !m_lock.test_and_set(std::memory_order_acquire);
        }

        auto const bound = panic_lock_ticks();
        auto const start = arch::Tick::get().count;
        while (m_lock.test_and_set(std::memory_order_acquire)) {
            if (arch::Tick::get().count - start >= bound) {
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
    return arch::Tick::get().count;
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
    // Read once, and the period taken from that one value: the rate goes from
    // 0 to its number while the system boots, and a second read could see a
    // number the first did not, so the divisor of us() below would be 0.
    auto const tick_hz = time::ticks_per_second();
    auto const period = tick_hz / static_cast<u64>(time::HZ);
    if (period == 0) {
        log::info("console: the tick counter's rate is not known yet, so no times can be given");
        return;
    }
    // Microseconds, from whole seconds and the part of a second left over
    // rather than from ticks * 1'000'000 / tick_hz. The counts are a total
    // over the whole boot, and a million times them wraps a 64-bit word once
    // they pass 1.8e13: about five hours of waiting at 1 GHz, one at 5 GHz.
    // The remainder is below tick_hz, and a million times that fits for any
    // rate up to about 1.8e13 Hz.
    auto const us = [tick_hz](u64 const ticks) -> u64 {
        return ticks / tick_hz * 1'000'000 + ticks % tick_hz * 1'000'000 / tick_hz;
    };
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
