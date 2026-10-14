#include <ours/arch/idle_thread.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/mm/cpu_local.hpp>

#include <atomic>

#include <gktl/counter.hpp>

// NOLINTBEGIN(readability-identifier-naming): named like every other GKTL_COUNTER in the tree.
GKTL_COUNTER(IDLE_MONITOR_WAKES, "idle.monitor_wakes");
// NOLINTEND(readability-identifier-naming)

namespace ours {
namespace {
    /// What a CPU's monitor word says: whether the CPU is waiting in MWAIT on
    /// it, or about to.
    enum MonitorState : u32 {
        NotWaiting = 0,
        Waiting = 1,
    };

    /// The span MONITOR watches. CPUID leaf 5 gives the real one; 64 bytes,
    /// a cache line, is what every x86 part this kernel runs on has.
    constexpr usize MONITOR_LINE = 64;

    /// The hint for C1, the shallowest state, the one HLT enters: deeper
    /// states are chosen by a later step, from the CPU's own table.
    constexpr u32 MWAIT_HINT_C1 = 0x00;

    /// A CPU's monitor word, alone on its line: MONITOR watches the whole
    /// line, so anything else on it that another CPU writes would end the
    /// wait for nothing. Zero is NotWaiting, so the per-CPU image needs no
    /// initialiser.
    struct alignas(MONITOR_LINE) MonitorLine {
        std::atomic<u32> state;
    };
    CPU_LOCAL MonitorLine MONITOR;
} // namespace

/// Without MONITOR, halt until the next interrupt, enabling interrupts as
/// part of the halt. `sti` takes effect after the instruction that follows
/// it, so `sti; hlt` cannot be split by an interrupt: the CPU either halts
/// with interrupts enabled or is woken by the very interrupt that arrived.
/// The caller decides there is nothing to run with interrupts disabled and
/// relies on this to close the window between that decision and the halt.
///
/// With MONITOR, the same wait in MWAIT, which also ends when another CPU
/// writes this CPU's monitor word, so a waker that finds the word saying
/// Waiting writes it instead of sending an IPI (wake()). The window between
/// the caller's decision and the wait is closed the same way, `sti; mwait`,
/// and one more is closed by the order below. A waker that comes before the
/// word says Waiting reads NotWaiting and sends its IPI, which waits, pending,
/// for the `sti`. One that comes after it but before MONITOR is armed has
/// written NotWaiting already, and the check after MONITOR sees it and does
/// not wait. One that comes after MONITOR is armed writes a watched line, and
/// MWAIT does not sleep or wakes at once.
///
/// Interrupts are left enabled on return; the caller's interrupt-state guard
/// restores whatever it had.
auto ArchIdleThread::idle() -> void {
    if (!FEATURE_HAS_MONITOR) {
        asm volatile("sti; hlt" ::: "memory");
        return;
    }

    auto &state = mm::CpuLocal::access(MONITOR).state;
    state.store(Waiting, std::memory_order_relaxed);
    asm volatile("monitor" : : "a"(&state), "c"(0), "d"(0) : "memory");
    if (state.load(std::memory_order_acquire) == Waiting) {
        asm volatile("sti; mwait" : : "a"(MWAIT_HINT_C1), "c"(0) : "memory");
    } else {
        asm volatile("sti" ::: "memory");
    }

    // Acquire, as the load above: a waker that wrote the word did so after
    // queueing whatever it woke this CPU for, and the caller is about to look.
    if (state.exchange(NotWaiting, std::memory_order_acquire) == NotWaiting) {
        IDLE_MONITOR_WAKES.add(1);
    }
}

/// Release: whatever the waker did before -- queued a thread, set a
/// transition -- is seen by the CPU once it reads the word.
auto ArchIdleThread::wake(CpuNum const cpu) -> bool {
    if (!FEATURE_HAS_MONITOR) {
        return false;
    }
    return mm::CpuLocal::access(MONITOR, cpu)
               .state.exchange(NotWaiting, std::memory_order_acq_rel) == Waiting;
}

auto ArchIdleThread::waits_in_mwait() -> bool {
    return FEATURE_HAS_MONITOR;
}

auto ArchIdleThread::num_monitor_wakes(CpuNum const cpu) -> u64 {
    return static_cast<u64>(IDLE_MONITOR_WAKES.get(cpu));
}
} // namespace ours
