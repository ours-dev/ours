#include <ours/arch/idle_thread.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/init.hpp>
#include <ours/mm/cpu_local.hpp>

#include <atomic>

#include <arch/x86/cpuid_observer.hpp>
#include <gktl/counter.hpp>
#include <gktl/init_hook.hpp>
#include <idle_states/idle_states.hpp>
#include <logz4/log.hpp>

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

    /// A CPU's monitor word, alone on its line: MONITOR watches the whole
    /// line, so anything else on it that another CPU writes would end the
    /// wait for nothing. Zero is NotWaiting, so the per-CPU image needs no
    /// initialiser.
    struct alignas(MONITOR_LINE) MonitorLine {
        std::atomic<u32> state;
    };
    CPU_LOCAL MonitorLine MONITOR;

    /// The states an idle CPU may wait in, the boot CPU's, which every CPU
    /// is taken to share: C1 alone until ArchIdleThread::find_states() has
    /// asked CPUID, and never written again after.
    idle_states::IdleStates IDLE_STATES;

    /// CPUID leaves 0, 1, 5 and 6, as idle_states wants them.
    struct VendorLeaf {
        static constexpr u32 LEAF = 0;
        static constexpr u32 SUBLEAF = 0;
        u32 max_leaf = 0;
        bool intel = false;
        auto observe(u32 const eax, u32 const ebx, u32 const ecx, u32 const edx) -> void {
            max_leaf = eax;
            // "GenuineIntel", in EBX, EDX, ECX.
            intel = ebx == 0x756e6547 && edx == 0x49656e69 && ecx == 0x6c65746e;
        }
    };
    struct SignatureLeaf {
        static constexpr u32 LEAF = 1;
        static constexpr u32 SUBLEAF = 0;
        u32 eax = 0;
        auto observe(u32 const a, u32, u32, u32) -> void {
            eax = a;
        }
    };
    struct ThermalAndPowerLeaf {
        static constexpr u32 LEAF = 6;
        static constexpr u32 SUBLEAF = 0;
        bool arat = false;
        auto observe(u32 const eax, u32, u32, u32) -> void {
            arat = (eax & 0x4) != 0;
        }
    };
    struct MwaitLeaf {
        static constexpr u32 LEAF = 5;
        static constexpr u32 SUBLEAF = 0;
        idle_states::MwaitLeaf leaf{};
        auto observe(u32, u32, u32 const ecx, u32 const edx) -> void {
            leaf = {.ecx = ecx, .edx = edx};
        }
    };

    /// Says which states an idle CPU waits in, once the console is up; they
    /// were found long before, on the boot CPU alone (find_states()).
    INIT_CODE
    auto log_idle_states() -> void {
        if (!FEATURE_HAS_MONITOR) {
            log::info("idle: HLT, C1");
            return;
        }
        log::info("idle: MWAIT, {} states", IDLE_STATES.states().size());
        for (auto const &state: IDLE_STATES.states()) {
            log::info("idle:   {} (hint {:#04x}, exit {} us){}", state.name, state.mwait_hint,
                      state.exit_latency_us, IDLE_STATES.is_enabled(state) ? "" : ", not enabled");
        }
    }
    GKTL_INIT_HOOK(LogIdleStates, log_idle_states, gktl::InitLevel::Platform);
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
auto ArchIdleThread::idle(Duration const last_idle, Duration const until_deadline) -> void {
    if (!FEATURE_HAS_MONITOR) {
        asm volatile("sti; hlt" ::: "memory");
        return;
    }

    auto const until_us =
        until_deadline == Duration::max()
            ? idle_states::IdleStates::NO_DEADLINE
            : static_cast<u64>(duration_cast<Microseconds>(until_deadline).count());
    auto const &chosen = IDLE_STATES.pick(
        static_cast<u64>(duration_cast<Microseconds>(last_idle).count()), until_us);

    auto &state = mm::CpuLocal::access(MONITOR).state;
    state.store(Waiting, std::memory_order_relaxed);
    asm volatile("monitor" : : "a"(&state), "c"(0), "d"(0) : "memory");
    if (state.load(std::memory_order_acquire) == Waiting) {
        asm volatile("sti; mwait" : : "a"(chosen.mwait_hint), "c"(0) : "memory");
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

/// Without MONITOR an idle CPU waits in HLT, and C1 is all there is.
INIT_CODE
auto ArchIdleThread::find_states() -> void {
    if (!FEATURE_HAS_MONITOR) {
        return;
    }
    auto vendor = VendorLeaf();
    arch::dispatch_cpuid(vendor);
    auto signature = SignatureLeaf();
    arch::dispatch_cpuid(signature);
    auto mwait = MwaitLeaf();
    if (vendor.max_leaf >= MwaitLeaf::LEAF) {
        arch::dispatch_cpuid(mwait);
    }
    auto power = ThermalAndPowerLeaf();
    if (vendor.max_leaf >= ThermalAndPowerLeaf::LEAF) {
        arch::dispatch_cpuid(power);
    }
    IDLE_STATES = idle_states::IdleStates::for_cpu({
        .intel = vendor.intel,
        .signature = idle_states::decode_signature(signature.eax),
        .mwait = mwait.leaf,
        .arat = power.arat,
    });
}

auto ArchIdleThread::waits_in_mwait() -> bool {
    return FEATURE_HAS_MONITOR;
}

auto ArchIdleThread::num_monitor_wakes(CpuNum const cpu) -> u64 {
    return static_cast<u64>(IDLE_MONITOR_WAKES.get(cpu));
}
} // namespace ours
