#include <ours/arch/time.hpp>
#include <ours/assert.hpp>
#include <ours/boot/options.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_hotplug_hook.hpp>
#include <ours/init.hpp>
#include <ours/irq/mod.hpp>
#include <ours/irq/types.hpp>
#include <ours/lockdep.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>
#include <ours/time/tick_broadcast.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/tick_sched.hpp>
#include <ours/time/timer_queue.hpp>

#include <logz4/log.hpp>

namespace ours::time {
namespace {
    /// The device a TickDevice points at before a real one is enrolled.
    ///
    /// It is a complete, legal ClockEventDevice rather than a null pointer, so
    /// that m_clkdev never needs a null check -- and it is built to lose every
    /// comparison check_new_device() makes, so that the selection code never
    /// needs to special-case it either: no features, so it is never the
    /// oneshot-capable incumbent that blocks a periodic-only newcomer; rating
    /// 0, below every real device; and an *empty* cpumask, which is neither
    /// per-CPU (so a global device may replace it) nor equal to any real
    /// device's mask (so check_preferred() prefers the newcomer); it is the
    /// shared cpu_none_mask(). The one place that still asks has_device() is
    /// rebind_device(), and that is a genuine semantic difference, not a null
    /// guard: a first binding has no deadline to inherit.
    ///
    /// Two more things about it are load-bearing. It carries an all-null
    /// vtable rather than a null vtable pointer, because rebind_device()
    /// detaches the outgoing device and every vtable wrapper dereferences
    /// `m_vtable` before checking the slot -- a null pointer there read
    /// garbage from the identity map instead of faulting. And it lives in
    /// ordinary .data, not INIT_DATA: the per-CPU TickDevice template is
    /// replicated into every CPU's unit with this pointer inside it, and a
    /// CPU that never gets a real device (or gets one only after the init
    /// sections are reclaimed) would otherwise be left holding a dangling
    /// pointer.
    constexpr ClockEventDevice::Vtable NOOP_VTABLE{};
    // Namespace-scope objects are UPPER_CASE throughout this tree (I8253_CLKEVT,
    // LEGACY_TIMER, HPET_MMIO); .clang-tidy's lower_case rule predates that.
    // NOLINTNEXTLINE(readability-identifier-naming)
    ClockEventDevice NOOP_DEVICE{"Noop Device", &NOOP_VTABLE, ClockEventDevice::Feature::None,
                                 ClockEventDevice::Rating(0), &cpu_none_mask()};
} // namespace

TickDevice::TickDevice()
    : m_canary(this), m_clkdev(&NOOP_DEVICE), m_mode(Mode::Periodic), m_group(nullptr) {}

/// Asks for an interrupt at |expired|, on behalf of this CPU's timer queue.
///
/// In periodic mode this is a no-op that reports success. The device is
/// already firing every 1/HZ and the queue is run from that tick, so there is
/// nothing to program -- and touching the device here would be harmful: a
/// periodic PIT's "next event" register is its period, and an APIC timer in
/// periodic mode reloads whatever count it was last given.
///
/// In oneshot mode INFINITE_TIME means "nothing pending", and the answer is
/// to stop the device -- ClockEventDevice::Mode::OneshotStopped -- not to
/// shut it down; the next real deadline wakes it again below. The first draft
/// compared against ustl::NumericLimits<TimePoint>::max(), which is the
/// unspecialised std::numeric_limits and returns a value-initialised
/// TimePoint: the epoch, so the "infinite" branch fired for time zero and
/// never for infinity.
auto TickDevice::program_event(TimePoint<> expired) -> Status {
    m_canary.verify<Self>();

    if (m_mode == Mode::Periodic) {
        return Status::Ok;
    }

    // A device shared with other CPUs is armed for the earliest of all their
    // deadlines, which only the group knows.
    if (m_group != nullptr) {
        return m_group->set_deadline(Cpu::id(), expired);
    }

    return m_clkdev->arm_or_park(expired);
}

auto TickDevice::has_device() const -> bool {
    return m_clkdev != &NOOP_DEVICE;
}

auto TickDevice::is_percpu(ClockEventDevice const &dev, CpuNum cpu) -> bool {
    return dev.cpumask() == CpuMask::of(cpu);
}

/// Whether |newdev| may serve as this CPU's tick device at all. One clause is
/// missing: a global device whose interrupt cannot be re-targeted to this CPU
/// should be refused too, but the IRQ layer here has no such query yet, and
/// irq::set_affinity() is a stub that says yes to everything, so the clause
/// would be vacuous. When affinity becomes real it belongs between the two
/// checks below.
auto TickDevice::check_percpu(ClockEventDevice const &newdev, CpuNum cpu) const -> bool {
    // A device that cannot interrupt this CPU is of no use to it.
    if (!newdev.cpumask().test(cpu)) {
        return false;
    }

    // A device that is this CPU's own is always admissible.
    if (is_percpu(newdev, cpu)) {
        return true;
    }

    // |newdev| is a global device. It may not displace a per-CPU one: the
    // per-CPU device costs nothing to keep and can never be taken away by
    // another CPU's needs, whereas a global device serves one CPU at a time.
    // (The placeholder's empty mask is not per-CPU, so it is displaceable.)
    if (is_percpu(*m_clkdev, cpu)) {
        return false;
    }

    return true;
}

/// Whether |newdev| is a better tick device than the current one. Two rules,
/// in order:
///
///  1. Oneshot capability is worth more than any rating. High-resolution
///     timers and a stopped idle tick both need it, so a periodic-only
///     device never replaces an oneshot-capable one, and is never installed
///     at all once this CPU has moved to Oneshot mode.
///
///  2. Otherwise the higher rating wins -- except that a device with a
///     different mask than the current one is preferred regardless. After
///     check_percpu() the only way the masks differ is that |newdev| is
///     per-CPU and the current device is global, and a CPU-local device is
///     always the better tick source whatever its rating says (the local
///     APIC is rated below the PIT and the HPET, and still belongs on every
///     CPU).
auto TickDevice::check_preferred(ClockEventDevice const &newdev) const -> bool {
    if (!newdev.has_feature(ClockEventDevice::Feature::Oneshot)) {
        if (m_clkdev->has_feature(ClockEventDevice::Feature::Oneshot)) {
            return false;
        }
        if (m_mode == Mode::Oneshot) {
            return false;
        }
    }

    // Against the placeholder both halves hold -- rating 0, empty mask -- so
    // the first real device is always taken without a special case.
    return newdev.rating() > m_clkdev->rating() || newdev.cpumask() != m_clkdev->cpumask();
}

auto TickDevice::check_new_device(ClockEventDevice *new_clkdev) -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(new_clkdev);
    DEBUG_ASSERT(arch::irqs_disabled(),
                 "a tick device is swapped with its interrupt possibly live");

    auto const cpu = Cpu::id();
    if (!check_percpu(*new_clkdev, cpu) || !check_preferred(*new_clkdev)) {
        log::info("CPU[{}] keeps tick device {} over {}", cpu, m_clkdev->name(),
                  new_clkdev->name());
        return Status::AlreadyBound;
    }

    return rebind_device(new_clkdev);
}

/// Swaps the devices, then puts the new one into service in the mode this CPU
/// is in.
///
/// A CPU that runs Oneshot changes devices through replace_with(), which also
/// puts right what its timer queue believes about the device. This used to
/// carry the old device's handler and pending deadline over by hand, and a
/// swap in Oneshot mode installed an empty handler, then left the queue's
/// record of what the device was armed for naming a deadline that the new
/// device had never been given.
auto TickDevice::rebind_device(ClockEventDevice *new_clkdev) -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(new_clkdev);
    DEBUG_ASSERT(arch::irqs_disabled(),
                 "a tick device is swapped with its interrupt possibly live");

    auto const cpu = Cpu::id();
    if (m_mode == Mode::Oneshot) {
        // A global device needs a group, which selection does not make.
        if (!is_percpu(*new_clkdev, cpu)) {
            return Status::Unsupported;
        }
        return replace_with(*new_clkdev, nullptr);
    }

    auto const percpu_mask = CpuMask::of(cpu);
    if (has_device()) {
        // The outgoing device is detached -- stopped, and with no handler, so
        // a straggling interrupt from it finds nothing to run. Taking the
        // handler is what leaves it empty.
        auto handler = m_clkdev->take_event_handler();
        auto _ = m_clkdev->switch_mode(ClockEventDevice::Mode::Detached);
    } else {
        // The first device this CPU ever gets.
        //
        // The first CPU to get a tick device becomes the one that advances
        // jiffies. It has to be claimed here and not fixed at CPU 0, because
        // whichever CPU has a running device is the only one guaranteed to
        // tick.
        if (ClockSource::Default::CPUNUM == INVALID_CPU_NUM) {
            ClockSource::Default::CPUNUM = cpu;
        }
    }

    m_clkdev = new_clkdev;
    auto _ = new_clkdev->switch_mode(ClockEventDevice::Mode::Shutdown);

    // A global device is steered at this CPU; the old code set the *old*
    // device's affinity here, to its own mask, which changed nothing.
    if (new_clkdev->cpumask() != percpu_mask && new_clkdev->irqnum() != irq::INVALID_V_IRQ_NUM) {
        auto _ = irq::set_affinity(new_clkdev->irqnum(), percpu_mask);
    }

    if (wants_oneshot_tick(*new_clkdev, cpu)) {
        auto const status = setup_oneshot_tick(new_clkdev);
        if (Status::Ok == status) {
            return status;
        }
        // The mode is still Periodic, so the CPU keeps a tick.
        log::error("CPU[{}] could not run {} one-shot ({}), keeping the periodic tick", cpu,
                   new_clkdev->name(), to_string(status));
    }
    return setup_periodic(new_clkdev);
}

/// Puts |dev| into service for the periodic tick. A device with native
/// periodic support is simply switched into it; one without (the local APIC
/// in TSC-deadline mode, the HPET comparators that lack Tn_PER_INT_CAP) runs
/// in oneshot and is re-armed one tick ahead from inside
/// handle_periodic_event(), so from above the two are indistinguishable.
auto TickDevice::setup_periodic(ClockEventDevice *dev) -> Status {
    dev->set_event_handler([](ClockEventDevice *device) { handle_periodic_event(device); });

    if (dev->has_feature(ClockEventDevice::Feature::Periodic)) {
        return dev->switch_mode(ClockEventDevice::Mode::Periodic);
    }

    auto const status = dev->switch_mode(ClockEventDevice::Mode::Oneshot);
    if (Status::Ok != status) {
        return status;
    }
    return dev->program_event(SteadyClock::now() + NSEC_PER_TICK);
}

auto TickDevice::wants_oneshot_tick(ClockEventDevice const &dev, CpuNum const cpu) const -> bool {
    return boot::options().time_oneshot() && is_percpu(dev, cpu) &&
           dev.has_feature(ClockEventDevice::Feature::Oneshot);
}

/// Enters Oneshot mode the way a CPU does it for good: on the CPU itself,
/// when its own device is enrolled, and not as a switch of a tick that is
/// already running.
///
/// The handler of the device this one replaces is not carried over, and
/// neither is its deadline. That handler is the periodic tick's, and the
/// deadline is a tick that the queue's own head takes the place of. What the
/// queue holds, including any timer armed before the device existed, is
/// armed from scratch by rearm_device(): until now programming was a no-op,
/// so the queue's record of what the device is armed for names a deadline
/// that was never written.
///
/// Interrupts are off for the handler, the mode and the first arming together
/// (the caller's contract, see check_new_device()). On the boot CPU the HPET
/// may still be interrupting while its replacement goes in, and an interrupt
/// between the mode and the handler would find the mode saying Oneshot and the
/// handler still the periodic one.
///
/// The mode is committed only after the device accepted Oneshot, so a failure
/// leaves this CPU Periodic, with |dev| bound but not yet in service, and
/// setup_periodic() still to run on it.
auto TickDevice::setup_oneshot_tick(ClockEventDevice *dev) -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(arch::irqs_disabled());

    dev->set_event_handler([](ClockEventDevice *) { handle_oneshot_event(); });
    auto const status = dev->switch_mode(ClockEventDevice::Mode::Oneshot);
    if (Status::Ok != status) {
        return status;
    }
    m_mode = Mode::Oneshot;
    log::info("CPU[{}] tick device {} runs one-shot", Cpu::id(), dev->name());

    TimerQueue::get().rearm_device();

    // The tick is a Timer in this queue from the moment the device is up, on
    // the boot CPU too. Until the scheduler is started it beats the lockup
    // detector's heart and advances jiffies, and nothing more.
    TickSched::get().arm_timer();
    return Status::Ok;
}

/// Not release(): that one detaches the device and forgets the group, and
/// the CPU coming back needs both. Shutdown keeps the handler, so an
/// interrupt the device had already raised finds an empty queue to run.
auto TickDevice::stop_for_offline() -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(arch::irqs_disabled());

    if (m_mode == Mode::Oneshot) {
        TickSched::get().cancel_timer();
    }
    if (m_group != nullptr) {
        m_group->leave(Cpu::id());
        return;
    }
    auto _ = m_clkdev->switch_mode(ClockEventDevice::Mode::Shutdown);
}

/// The same paths a device takes into service in the first place:
/// setup_periodic() and setup_oneshot_tick(), or the group's join() followed
/// by what setup_oneshot_tick() does for the queue and the tick.
auto TickDevice::restart_after_offline() -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(arch::irqs_disabled());

    if (m_mode == Mode::Periodic) {
        return setup_periodic(m_clkdev);
    }
    if (m_group == nullptr) {
        return setup_oneshot_tick(m_clkdev);
    }
    if (auto const status = m_group->join(Cpu::id()); status != Status::Ok) {
        return status;
    }
    TimerQueue::get().rearm_device();
    TickSched::get().arm_timer();
    return Status::Ok;
}

namespace {
    /// The Tick level's CPU hotplug hook (ours/cpu_hotplug_hook.hpp): the
    /// lowest level, so it stops last on the way down and starts first on
    /// the way up, the levels above arming timers on it.
    auto stop_tick_for_offline() -> void {
        TickDevice::Current::get().stop_for_offline();
    }

    auto restart_tick_after_offline() -> void {
        if (auto const status = TickDevice::Current::get().restart_after_offline();
            status != Status::Ok) {
            log::error("CPU[{}] came back without a tick: {}", Cpu::id(), to_string(status));
        }
    }
} // namespace

CPU_HOTPLUG_HOOK(Tick, ::ours::CpuHotplugLevel::Tick, &stop_tick_for_offline,
                 &restart_tick_after_offline)

auto TickDevice::release() -> void {
    if (m_group != nullptr) {
        m_group->leave(Cpu::id());
        m_group = nullptr;
        return;
    }

    // Detached: stopped, and with no handler, so a straggling interrupt from
    // it finds nothing to run.
    auto handler = m_clkdev->take_event_handler();
    auto _ = m_clkdev->switch_mode(ClockEventDevice::Mode::Detached);
}

auto TickDevice::adopt(ClockEventDevice &dev, TickBroadcast *group) -> Status {
    if (group != nullptr) {
        // The device is kept twice, here and in the group, and has to be the
        // same one.
        DEBUG_ASSERT(&group->device() == &dev, "{} is not the device of the group it is given",
                     dev.name());
        auto const status = group->join(Cpu::id());
        if (Status::Ok != status) {
            return status;
        }
        m_group = group;
        m_clkdev = &dev;
        return Status::Ok;
    }

    dev.set_event_handler([](ClockEventDevice *) { handle_oneshot_event(); });
    auto _ = dev.switch_mode(ClockEventDevice::Mode::Shutdown);
    auto const status = dev.switch_mode(ClockEventDevice::Mode::Oneshot);
    if (Status::Ok != status) {
        auto handler = dev.take_event_handler();
        return status;
    }
    m_clkdev = &dev;
    return Status::Ok;
}

/// The timer queue is not touched until the new device is in service: it is
/// the queue's record of what the device is armed for that goes stale when
/// the device changes, and rearm_device() is what puts it right.
///
/// The tick is a Timer in the queue and stays one. Nothing about it depends
/// on which device interrupts for it, so nothing is done to it here.
auto TickDevice::replace_with(ClockEventDevice &dev, TickBroadcast *group) -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(arch::irqs_disabled(),
                 "a tick device is swapped with its interrupt possibly live");
    DEBUG_ASSERT(m_mode == Mode::Oneshot, "only a CPU that runs Oneshot replaces its device");
    DEBUG_ASSERT(has_device());

    auto const cpu = Cpu::id();
    DEBUG_ASSERT((group == nullptr) == is_percpu(dev, cpu),
                 "{} for CPU[{}]: its own device {}, given a group {}", dev.name(), cpu,
                 is_percpu(dev, cpu), group != nullptr);

    auto &left = *m_clkdev;
    auto *const left_group = m_group;

    release();
    auto const status = adopt(dev, group);
    if (Status::Ok != status) {
        // |dev| would not take the tick, so it is out, and before anything
        // else is tried. Putting |left| back arms it again, and when |left| is
        // refused too that comes back to on_device_refused() from inside this
        // call; had |dev| not been marked by then, that would choose it again,
        // for ever.
        dev.mark_failed();

        // A device that has failed is not worth going back to: it is what the
        // caller is moving the tick away from. The CPU stays without one, and
        // the caller goes on to the next.
        if (left.is_failed()) {
            log::warn("CPU[{}] could not move its tick to {} ({}), and {} has failed", cpu,
                      dev.name(), to_string(status), left.name());
            return status;
        }

        // Back where it was. Best effort: if this does not work either, the
        // CPU has no tick device and says so.
        auto const back = adopt(left, left_group);
        if (Status::Ok != back) {
            log::error("CPU[{}] could not move its tick to {} ({}) and could not go back to {} "
                       "({})",
                       cpu, dev.name(), to_string(status), left.name(), to_string(back));
        }
        TimerQueue::get().rearm_device();
        return status;
    }

    // Before the queue arms the device: it may refuse, and the replacement for
    // it is then logged by that call, so this has to come first to be read in
    // the order it happened.
    log::info("CPU[{}] tick moved from {} to {}", cpu, left.name(), dev.name());
    TimerQueue::get().rearm_device();
    return Status::Ok;
}

auto TickDevice::on_device_refused() -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(arch::irqs_disabled(),
                 "a tick device is swapped with its interrupt possibly live");

    auto const cpu = Cpu::id();

    // What the queue was talking to. A group's device is the one in service
    // for every member, so it is marked failed for all of them, and each
    // member finds out when its own next deadline is refused.
    auto &device = *m_clkdev;
    log::warn("CPU[{}] tick device {} refused a deadline", cpu, device.name());

    // Once is not proof that the device is gone. Ask again, straight away;
    // only a device that refuses every time is retired. A refusal of one of
    // these asks is counted like the first, and does not come back here: the
    // answer is returned to this loop.
    for (u32 attempt = 1; attempt < ATTEMPTS_BEFORE_RETIRING; ++attempt) {
        if (TimerQueue::get().try_rearm_device()) {
            log::info("CPU[{}] tick device {} took the deadline at attempt {}", cpu, device.name(),
                      attempt + 1);
            return;
        }
    }
    log::warn("CPU[{}] tick device {} refused {} deadlines in a row and is retired", cpu,
              device.name(), ATTEMPTS_BEFORE_RETIRING);
    device.mark_failed();

    // Moving the tick arms the new device, and the new device may refuse
    // too: that comes back here through the queue, from inside replace_with(),
    // marks the new device failed and moves on.
    //
    // Why this ends. Every device that is chosen is either put in service, or
    // marked failed by replace_with() before it returns, and a failed device
    // is never chosen (pick_replacement()). The asks above are a loop of
    // two, not a recursion. A round that does not end the loop
    // therefore leaves one device fewer to choose from, and the devices are a
    // fixed, finite list. The same measure bounds the calls nested through
    // replace_with() and rearm_device(): each one is made after a device was
    // marked failed.
    while (m_clkdev->is_failed()) {
        auto const pick = pick_replacement(cpu);
        if (pick.device == nullptr) {
            log::error("CPU[{}] tick device {} failed and no other device can serve it; the CPU "
                       "has no tick",
                       cpu, m_clkdev->name());
            return;
        }

        // A device that would not take the tick is marked failed by
        // replace_with(), and the next round chooses another.
        if (Status::Ok != replace_with(*pick.device, pick.group)) {
            log::warn("CPU[{}] {} would not take the tick", cpu, pick.device->name());
        }
    }
}

auto TickDevice::pick_replacement(CpuNum cpu) -> Replacement {
    struct Choice {
        CpuNum cpu;
        ClockEventDevice *own;
        ClockEventDevice *global;
    };
    Choice choice{cpu, nullptr, nullptr};

    ClockEventDevice::for_each_enrolled(
        [](ClockEventDevice &device, void *context) -> bool {
            auto &choice = *static_cast<Choice *>(context);
            if (device.is_failed() || !device.has_feature(ClockEventDevice::Feature::Oneshot) ||
                !device.cpumask().test(choice.cpu)) {
                return true;
            }

            if (is_percpu(device, choice.cpu)) {
                if (choice.own == nullptr || device.rating() > choice.own->rating()) {
                    choice.own = &device;
                }
            } else if (&device == global_clock_event()) {
                choice.global = &device;
            }
            return true;
        },
        &choice);

    if (choice.own != nullptr) {
        return {choice.own, nullptr};
    }
    if (choice.global != nullptr) {
        if (auto *const group = TickBroadcast::for_device(*choice.global); group != nullptr) {
            return {choice.global, group};
        }
    }
    return {};
}

/// The periodic tick's interrupt handler.
///
/// Three things happen, in this order:
///
///  1. do_timer(): jiffies advances. It is one global count, so exactly one
///     CPU -- ClockSource::Default::CPUNUM, claimed by the first CPU to bind
///     a device -- moves it. Before this check every CPU's device advanced
///     it, and on an N-CPU machine jiffies ran N times fast.
///
///  2. TickSched::on_tick(): the scheduler's tick. That is where the running
///     thread is charged and where local timers run; TickSched knows whether
///     there is a scheduler yet.
///
///  3. For a device emulating periodic mode in oneshot, the next tick is
///     armed. It is scheduled from the previous *deadline*, not from now, so
///     interrupt latency does not accumulate into drift.
///
/// Step 3 is also where missed ticks are made up, and *how* is what decides
/// whether this handler is stable. When the handler runs more than a period
/// late, "previous deadline plus one period" is already in the past. This
/// used to be programmed anyway, with the forcing program_event() clamping it
/// to the device's minimum delta -- so the missed tick was made up by another
/// interrupt, a few microseconds later, and the next one by another after
/// that. That only converges while one tick interrupt costs less than a
/// period. Once it costs more, every make-up interrupt leaves the deadline
/// further behind than it found it, interrupts arrive back to back, and the
/// thread they land on gets the minimum delta between them and nothing else.
/// Nothing in the loop sheds load, so it never recovers: this is what hung
/// dbg_1's fault_probe, whose user thread on cpu0 was interrupted forever by
/// ticks made expensive by a trace line per ring-3 interrupt and a serial
/// port shared with three other CPUs.
///
/// So the next deadline is armed with LateDeadline::Reject, and a deadline
/// that is already past comes back as TimeOut instead of as an interrupt. The
/// missed period is then counted right here and the deadline moved on by one
/// more, until it is ahead of now. However late this handler ran, the
/// interrupt it leaves armed is in the future, and the time until it belongs
/// to whatever was interrupted.
///
/// A missed period advances jiffies and nothing else. Running the whole tick
/// once per missed period, on_tick() included, would be right only for
/// CPU-time accounting that is tick-counted, charging one tick's worth per
/// call. Nothing under on_tick() counts calls that way: the scheduler charges
/// the clock delta since its last update, and the timer queue runs everything
/// whose deadline is at or before now, so the one call above has already
/// covered all the time that passed; a second call would find nothing to do.
/// The profiler's tick source does count calls, and would record the one
/// interrupted context several times over at one instant -- fabricated
/// samples, not caught-up ones. And the loop must stay cheap to be sure to
/// end. It ends because each pass costs a few instructions while moving the
/// deadline a whole period; with on_tick() inside it, whose cost has no
/// bound, it would end only while a tick costs less than a period -- the very
/// condition whose failure this loop exists to survive, moved inside one
/// interrupt with interrupts off.
///
/// jiffies itself does have to count every period: it is a count of periods,
/// and it is what the local APIC is calibrated against.
///
/// The loop also relies on advancing jiffies not advancing the clock that
/// program_event() measures "now" by: with jiffies as the clock source, each
/// pass would move now along with the deadline and the loop would never get
/// ahead. Here jiffies is not a ClockSource at all --
/// ClockSource::Default::init() is not implemented -- and SteadyClock reads
/// the HPET, or zero before one exists, which leaves every deadline ahead.
/// Enrolling jiffies as a source is the change that would need a guard here,
/// making up missed periods only while the clock source is not jiffies.
auto TickDevice::handle_periodic_event(ClockEventDevice *device) -> void {
    auto const cpu = Cpu::id();
    auto const do_timer = [cpu] {
        if (cpu == ClockSource::Default::CPUNUM) {
            ClockSource::Default::advance_ticks(1);
        }
    };

    // The deadline this interrupt answers, read before on_tick(): every later
    // tick is counted from it, and nothing that on_tick() does may move that
    // base.
    auto next = device->next_event();

    do_timer();
    TickSched::get().on_tick(SteadyClock::now());

    if (!device->is_mode_oneshot()) {
        return;
    }

    for (;;) {
        next += NSEC_PER_TICK;
        auto const status = device->program_event(next, ClockEventDevice::LateDeadline::Reject);
        if (Status::TimeOut != status) {
            DEBUG_ASSERT(Status::Ok == status,
                         "Failed to re-arm the emulated periodic tick on CPU[{}]", cpu);
            return;
        }
        do_timer();
    }
}

/// The device fired for the earliest deadline of this CPU's timer queue.
/// Everything that is due runs, the tick among it when its time has come, and
/// the queue arms the device again for what remains.
///
/// Nothing here advances jiffies or calls TickSched::on_tick(): the tick is a
/// timer in the queue like any other, and its handler does both.
auto TickDevice::handle_oneshot_event() -> void {
    TimerQueue::get().run_expired_timers();
}

} // namespace ours::time