#include <ours/arch/mp.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/ipi_service.hpp>
#include <ours/koid.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/cpu_hotplug.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer_queue.hpp>

#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/counter.hpp>
#include <ktl/name.hpp>
#include <logz4/log.hpp>

// NOLINTNEXTLINE(readability-identifier-naming): named like every other GKTL_COUNTER in the tree.
GKTL_COUNTER(IDLE_HALTS, "idle.halts");

namespace ours::task {
using mm::CpuLocal;

namespace {
    /// This CPU's time halted, as other CPUs read it: the halts that have
    /// ended, and when the one in progress began, so that a reader counts the
    /// part of it already gone. Without that, a halt that began before a
    /// window and ended in it counted whole, and one still going at its end
    /// not at all: up to a halt's length of error at each end, which is a
    /// tick's 10 ms now and a second once the tick is gone.
    ///
    /// Written only by its own CPU, in the idle loop with interrupts off;
    /// read by any CPU. The two fields change together, so they are guarded
    /// by a sequence count: the writer makes it odd before it changes them
    /// and even after, and a reader that saw it odd, or saw it change
    /// across its reads, reads again. Every field is an atomic, so a read
    /// that loses the race is a retry, never a torn value.
    struct HaltClock {
        std::atomic<u32> sequence;
        /// The halts that have ended, in nanoseconds.
        std::atomic<i64> halted_ns;
        /// When the halt in progress began, in nanoseconds of SteadyClock;
        /// 0 when none is.
        std::atomic<i64> since_ns;
    };

    CPU_LOCAL
    constinit HaltClock HALT_CLOCK{};

    auto ns_of(TimePoint<> const at) -> i64 {
        return static_cast<i64>(Nanoseconds(at.time_since_epoch()).count());
    }

    /// Opens a write: the sequence goes odd, and the release fence keeps the
    /// field stores that follow from being seen before it.
    auto begin_write(HaltClock &clock) -> u32 {
        auto const sequence = clock.sequence.load(std::memory_order_relaxed) + 1;
        clock.sequence.store(sequence, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        return sequence;
    }

    /// Closes it: the sequence goes even, published after the field stores.
    auto end_write(HaltClock &clock, u32 const sequence) -> void {
        clock.sequence.store(sequence + 1, std::memory_order_release);
    }

    /// Records that a halt begins now, and answers when that is.
    auto start_halt() -> TimePoint<> {
        auto &clock = CpuLocal::access(HALT_CLOCK);
        auto const now = time::SteadyClock::now();
        auto const sequence = begin_write(clock);
        clock.since_ns.store(ns_of(now), std::memory_order_relaxed);
        end_write(clock, sequence);
        return now;
    }

    /// Records that the halt has ended, and answers how long it lasted.
    auto end_halt() -> Duration {
        auto &clock = CpuLocal::access(HALT_CLOCK);
        auto const sequence = begin_write(clock);
        auto const since = clock.since_ns.load(std::memory_order_relaxed);
        auto const halted = clock.halted_ns.load(std::memory_order_relaxed);
        auto const lasted = ns_of(time::SteadyClock::now()) - since;
        clock.halted_ns.store(halted + lasted, std::memory_order_relaxed);
        clock.since_ns.store(0, std::memory_order_relaxed);
        end_write(clock, sequence);
        return Nanoseconds(lasted);
    }

    /// How long from |now| until this CPU's device is due to wake it:
    /// Duration::max() when nothing is due, zero when the deadline has passed
    /// or is not known (TimerQueue::armed_deadline()).
    auto until_next_deadline(TimePoint<> const now) -> Duration {
        auto const deadline = time::TimerQueue::get().armed_deadline();
        if (deadline == INFINITE_TIME) {
            return Duration::max();
        }
        if (deadline <= now) {
            return Duration::zero();
        }
        return Duration(deadline - now);
    }
} // namespace

auto IdleThread::num_halts(CpuNum const cpu) -> u64 {
    return static_cast<u64>(IDLE_HALTS.get(cpu));
}

/// The acquire load of the sequence orders the field loads after it, and the
/// acquire fence orders them before the second load of the sequence: if both
/// loads see the same even count, no write overlapped the reads.
auto IdleThread::halted_time(CpuNum const cpu, TimePoint<> const now) -> Duration {
    auto const &clock = CpuLocal::access(HALT_CLOCK, cpu);
    for (;;) {
        auto const before = clock.sequence.load(std::memory_order_acquire);
        if ((before & 1) != 0) {
            arch::spin_hint();
            continue;
        }
        auto const halted = clock.halted_ns.load(std::memory_order_relaxed);
        auto const since = clock.since_ns.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (clock.sequence.load(std::memory_order_relaxed) != before) {
            continue;
        }
        auto const at = ns_of(now);
        auto in_progress = i64{0};
        if (since != 0 && at > since) {
            in_progress = at - since;
        }
        return Nanoseconds(halted + in_progress);
    }
}

// Ordered as the members are declared; m_thread and m_arch used to be listed
// the other way round, which -Wreorder rejects under -Werror.
IdleThread::IdleThread(CpuNum cpunum)
    : m_states({.current = State::Booting, .target = State::Running})
    , m_cpu(cpunum)
    , m_arch()
    , m_thread()
    , m_transitioned()
    , m_halted(false)
    , m_last_halt() {}

// |cpunum| is the CPU this idler belongs to, which is not necessarily the one
// running this code: the boot CPU sets up every CPU's idler during task::init().
auto IdleThread::init(CpuNum cpunum) -> void {
    new (this) IdleThread(cpunum);

    constexpr auto dummy_start_routine = [](void *) -> i32 {
        auto const cpunum = Cpu::id();
        panic("Idler[{}] enter the dummy start routine", cpunum);
    };

    constexpr auto dummy_trampoline_routine = [] [[noreturn]]
                                              () -> void {
        auto const cpunum = Cpu::id();
        panic("Idler[{}] enter the dummy trampoline routine", cpunum);
    };

    char buffer[24];
    auto const name = ktl::format_name(buffer, "Idle/{}", m_cpu);

    // TODO(SmallHuaZi) Assign the idle thread an unique Tid.
    //
    // |m_cpu| is handed over as the target CPU because an idler's CPU is not a
    // placement decision to be made for it: there is one per CPU and this is
    // that CPU's. Letting the scheduler choose was both wrong and unanswerable
    // -- wrong because the running CPU is the boot CPU for every iteration of
    // task::init()'s loop, so all of them landed on the BSP, and unanswerable
    // because selection only ever names an online CPU while this loop runs
    // over cpu_possible_mask() before the secondaries are up.
    [[maybe_unused]]
    auto const status = Thread::spawn_etc(KoIdGenerator::generate(), name, dummy_start_routine,
                                          nullptr, sched::BaseProfile(sched::IDLE_PRIORITY),
                                          dummy_trampoline_routine, m_thread.data(), m_cpu);
    DEBUG_ASSERT(Status::Ok == status, "Failed to initialize idler[{}]", m_cpu);

    m_thread->set_state_running();

    // Nobody else can see the thread yet, so the lock orders nothing; it is
    // taken because the flag setters require it and an exemption for "not
    // published yet" is exactly the kind of reasoning the analysis exists to
    // replace.
    {
        Guard<sync::SpinLock, IrqSave> guard{m_thread->get_pi_lock()};
        m_thread->set_flag_idle();
        m_thread->set_flag_nofree();
    }
}

auto IdleThread::run() -> void {
    // The idler is Running from the moment it runs. The state machine was
    // constructed {Booting, Running} and nothing ever advanced it, so
    // `current != target` held forever and the old loop rescheduled on every
    // iteration -- with interrupts and preemption both disabled, which is
    // exactly the state in which Scheduler::Current::reschedule() refuses
    // (add_pending() and return), so the CPU spun here silently with
    // interrupts off and no tick could ever arrive.
    m_states.store({.current = State::Running, .target = State::Running},
                   std::memory_order_release);

    auto &scheduler = Scheduler::Current::get();
    for (;;) {
        // Whether there is work is decided with interrupts off, so a wakeup
        // cannot land between the check and the halt and be slept through:
        // idle() re-enables interrupts only as part of the halt itself
        // (`sti; hlt`), which is atomic against a pending interrupt.
        {
            arch::AutoInterruptDisabler irq_disabler;
            // Before anything else: the scheduler switched here, runnable
            // threads or not, because a transition is due. The only one a
            // running CPU can be asked for is Offline. Withdrawn before the
            // claim, it leaves nothing to do here.
            if (has_pending_transition() &&
                claim({.current = State::Running, .target = State::Offline}, State::Leaving)) {
                go_offline_and_halt();
            }
            // Nothing queued here: before sleeping, take a thread that waits
            // behind another on a busier CPU. A CPU looks when it goes idle
            // and when it has just come back (come_back() enters this loop
            // without picking a thread, so a look in the pick would miss it).
            // One that has already halted takes no tick and looks again only
            // when an interrupt wakes it (item 23 of
            // docs/repairs/261103/kernel-task-sched.md).
            if (!scheduler.num_runnable() && !Scheduler::Current::steal()) {
                // Preemption off across the halt, so that the interrupt
                // ending it does not switch away from here on its way out:
                // the time read after the halt is then the halt's end, not
                // the end of whatever ran next. A preemption it asked for is
                // taken as this goes, after the count.
                AutoPreemptDisabler const no_preemption;
                auto const now = start_halt();
                m_arch.idle(m_last_halt, until_next_deadline(now));
                m_last_halt = end_halt();
                IDLE_HALTS.add(1);
                continue;
            }
        }

        // Something is runnable. Reschedule with interrupts and preemption
        // enabled -- Scheduler::Current::reschedule() will not switch from a
        // non-preemptible context, it only records a pending request -- and
        // let the next thread run; we are back here when it is our turn again.
        Thread::Current::reschedule();
    }
}

auto IdleThread::claim(StateMachine asked, State const on_the_way) -> bool {
    return m_states.compare_exchange_strong(asked, {.current = on_the_way, .target = asked.target},
                                            std::memory_order_acq_rel, std::memory_order_acquire);
}

/// Offline needs this CPU to run its idle thread, which the next reschedule
/// there does; Running needs the halted CPU started again. A start that
/// fails leaves the CPU offline and the target as it was.
///
/// The CPU and the caller race at |deadline|: the CPU to claim the
/// transition (claim()), the caller to withdraw it. Both are a
/// compare-and-swap from the state asked for, so exactly one succeeds. A
/// caller that withdrew may leave: the CPU, when it looks, finds nothing
/// asked of it. A caller that lost has a CPU already on its way, past the
/// point of turning back, and waits for it to arrive. Without the race the
/// two could each believe the other's answer: a start given up on and a
/// CPU online behind the caller's back, with nobody starting its Dpc
/// workers; or a CPU asked to go offline that never could, and a caller
/// waiting for it for ever.
auto IdleThread::transition(State const target, time::Deadline const deadline) -> Status {
    DEBUG_ASSERT(target == State::Offline || target == State::Running);
    auto const states = m_states.load(std::memory_order_acquire);
    if (states.current == State::Booting || states.current != states.target) {
        return Status::BadState;
    }
    if (states.current == target) {
        return Status::Ok;
    }

    auto const asked = StateMachine{.current = states.current, .target = target};
    m_transitioned.unsignal();
    m_states.store(asked, std::memory_order_release);
    if (target == State::Offline) {
        // An IPI to any other CPU; on its own CPU the caller is about to
        // block in the wait below, which reschedules there anyway.
        IpiService::send_resched(CpuMask::of(m_cpu));
    } else {
        // Raised again only by a halt after this start: going offline, or
        // arriving after the start is withdrawn.
        m_halted.store(false, std::memory_order_relaxed);
        if (auto const started = arch_activate_cpu(m_cpu); started != Status::Ok) {
            m_states.store(states, std::memory_order_release);
            return started;
        }
    }

    auto const waited = m_transitioned.wait(deadline);
    if (waited != Status::TimeOut) {
        return waited;
    }
    auto withdrawn = asked;
    if (m_states.compare_exchange_strong(withdrawn, states, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        return Status::TimeOut;
    }
    return m_transitioned.wait(time::Deadline::infinite());
}

/// The CPU's own two steps first, then every subsystem's (run_offline_hooks()):
///
///   1. no thread placed here, and the queued ones moved (deactivate());
///   2. out of the online mask, which every IPI and TLB shootdown sender
///      reads first;
///   3. the hooks, from the highest level down (ours/cpu_hotplug_hook.hpp):
///      the IPI mailbox closed, for a sender that read the mask just before;
///      the CPU's own timers cancelled; then the tick stopped, so that the
///      queue another CPU takes over holds only timers that belong to it.
///
/// Then the halt, whose flag is the last thing written: the control thread
/// resets the CPU with an INIT once it sees it, and a CPU reset while it held
/// a lock, here in the signal or the log, would hold it for ever.
///
/// The state is Leaving throughout, from run()'s claim: transition() can no
/// longer withdraw, and waits for the signal below.
auto IdleThread::go_offline_and_halt() -> void {
    DEBUG_ASSERT(arch::irqs_disabled());
    DEBUG_ASSERT(Cpu::id() == m_cpu);

    Scheduler::Current::deactivate();
    Cpu::set_state(Cpu::State::Online, false);
    run_offline_hooks();
    m_states.store({.current = State::Offline, .target = State::Offline},
                   std::memory_order_release);
    log::info("CPU[{}] is offline", m_cpu);
    m_transitioned.signal();
    arch_cpu_halt_offline(m_halted);
}

/// Going offline in reverse: the hooks from the lowest level up, then the
/// online mask and Active. INIT has flushed the TLB, so the shootdowns the
/// CPU missed while away are not missed. Interrupts go on before the loop,
/// as Thread::Current::become_idle() turns them on at boot; this thread's
/// preemption state is as the loop left it when the CPU halted.
///
/// The claim comes before anything is touched. A CPU that arrives after
/// transition() gave up on it finds the start withdrawn and halts again,
/// as it was before the start, taking no lock on the way: the next start
/// resets it with an INIT, which must not catch it holding one.
auto IdleThread::come_back() -> void {
    DEBUG_ASSERT(arch::irqs_disabled());
    DEBUG_ASSERT(Cpu::id() == m_cpu);

    if (!claim({.current = State::Offline, .target = State::Running}, State::Booting)) {
        arch_cpu_halt_offline(m_halted);
    }
    run_online_hooks();
    Cpu::set_state(Cpu::State::Online, true);
    Scheduler::Current::reactivate();
    m_states.store({.current = State::Running, .target = State::Running},
                   std::memory_order_release);
    log::info("CPU[{}] is online", m_cpu);
    m_transitioned.signal();

    arch::local_irq_enable();
    run();
    panic("CPU[{}]'s idle loop returned", m_cpu);
}

} // namespace ours::task