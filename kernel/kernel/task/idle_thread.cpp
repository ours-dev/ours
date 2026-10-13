#include <ours/arch/mp.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/ipi_service.hpp>
#include <ours/koid.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>

#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <ktl/name.hpp>
#include <logz4/log.hpp>
#include <sampler/hotplug.hpp>

namespace ours::task {
using mm::CpuLocal;

// Ordered as the members are declared; m_thread and m_arch used to be listed
// the other way round, which -Wreorder rejects under -Werror.
IdleThread::IdleThread(CpuNum cpunum)
    : m_states({.current = State::Booting, .target = State::Running})
    , m_cpu(cpunum)
    , m_arch()
    , m_thread()
    , m_cumulative_idle_time()
    , m_transitioned()
    , m_halted(false) {}

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
            // threads or not, because a transition is due.
            if (has_pending_transition()) {
                go_offline_and_halt();
            }
            if (!scheduler.num_runnable()) {
                auto const start_time = time::SteadyClock::now();
                m_arch.idle();
                m_cumulative_idle_time += time::SteadyClock::now() - start_time;
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

/// Offline needs this CPU to run its idle thread, which the next reschedule
/// there does; Running needs the halted CPU started again. A start that
/// fails leaves the CPU offline and the target as it was.
auto IdleThread::transition(State const target, time::Deadline const deadline) -> Status {
    DEBUG_ASSERT(target == State::Offline || target == State::Running);
    auto const states = m_states.load(std::memory_order_acquire);
    if (states.current == State::Booting || states.current != states.target) {
        return Status::BadState;
    }
    if (states.current == target) {
        return Status::Ok;
    }

    m_transitioned.unsignal();
    m_states.store({.current = states.current, .target = target}, std::memory_order_release);
    if (target == State::Offline) {
        // An IPI to any other CPU; on its own CPU the caller is about to
        // block in the wait below, which reschedules there anyway.
        IpiService::send_resched(CpuMask::of(m_cpu));
    } else if (auto const started = arch_activate_cpu(m_cpu); started != Status::Ok) {
        m_states.store(states, std::memory_order_release);
        return started;
    }
    return m_transitioned.wait(deadline);
}

/// The order is the order of what could still reach the CPU, each closed
/// before what depends on it:
///
///   1. no thread placed here, and the queued ones moved (deactivate());
///   2. out of the online mask, which every IPI and TLB shootdown sender
///      reads first;
///   3. the IPI mailbox closed, after the mask, for a sender that read the
///      mask just before: what it queued runs here now, and anything later
///      is answered for by its sender;
///   4. the tick stopped, its Timer cancelled, and the sampling timer
///      cancelled before it, so that the queue another CPU takes over holds
///      only timers that belong to it.
///
/// Then the halt, whose flag is the last thing written: the control thread
/// resets the CPU with an INIT once it sees it, and a CPU reset while it held
/// a lock, here in the signal or the log, would hold it for ever.
auto IdleThread::go_offline_and_halt() -> void {
    DEBUG_ASSERT(arch::irqs_disabled());
    DEBUG_ASSERT(Cpu::id() == m_cpu);

    Scheduler::Current::deactivate();
    Cpu::set_state(Cpu::State::Online, false);
    IpiService::close_mailbox();
    sampler::on_cpu_offline();
    time::TickDevice::Current::get().stop_for_offline();
    m_states.store({.current = State::Offline, .target = State::Offline},
                   std::memory_order_release);
    log::info("CPU[{}] is offline", m_cpu);
    m_transitioned.signal();
    arch_cpu_halt_offline(m_halted);
}

/// Going offline in reverse. INIT has flushed the TLB, so the shootdowns the
/// CPU missed while away are not missed. Interrupts go on before the loop,
/// as Thread::Current::become_idle() turns them on at boot; this thread's
/// preemption state is as the loop left it when the CPU halted.
auto IdleThread::come_back() -> void {
    DEBUG_ASSERT(arch::irqs_disabled());
    DEBUG_ASSERT(Cpu::id() == m_cpu);

    m_halted.store(false, std::memory_order_relaxed);
    if (auto const status = time::TickDevice::Current::get().restart_after_offline();
        status != Status::Ok) {
        log::error("CPU[{}] came back without a tick: {}", m_cpu, to_string(status));
    }
    sampler::on_cpu_online();
    IpiService::open_mailbox();
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