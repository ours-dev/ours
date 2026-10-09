#include <ours/arch/apic.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/cpu.hpp>
#include <ours/irq/chip.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/clock_event_device.hpp>
#include <ours/time/clock_source.hpp>

#include <algorithm>
#include <array>

#include <arch/interrupt.hpp>
#include <arch/spin_hint.hpp>
#include <arch/tick.hpp>
#include <arch/x86/apic/xapic.hpp>
#include <arch/x86/msr.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ustl/bit.hpp>
#include <ustl/limits.hpp>

namespace ours {
namespace {
    bool X2APIC_ENABLED = false;

    CPU_LOCAL
    u32 APIC_ID;

    constinit auto CPU_TO_APICID = []() constexpr -> std::array<u32, MAX_CPU> {
        std::array<u32, MAX_CPU> apicids{};
        for (auto &apicid: apicids) {
            apicid = arch::INVALID_APIC_ID;
        }
        return apicids;
    }();

    constinit auto APICID_TO_CPU = []() constexpr -> std::array<CpuNum, MAX_CPU> {
        std::array<CpuNum, MAX_CPU> cpunums{};
        for (auto &cpunum: cpunums) {
            cpunum = INVALID_CPU_NUM;
        }
        return cpunums;
    }();
} // namespace

auto current_apic_id() -> u32 {
    return mm::CpuLocal::read(APIC_ID);
}

using namespace irq;
using namespace time;
using mm::CpuLocal;

INIT_CODE
auto assign_cpunum_for_apic(u32 apicid) -> CpuNum {
    INIT_DATA
    static CpuMask ASSIGNED;

    if (apicid >= MAX_CPU) {
        return INVALID_CPU_NUM;
    }

    if (APICID_TO_CPU[apicid] != INVALID_CPU_NUM) {
        return APICID_TO_CPU[apicid];
    }

    auto const assigned_count = ASSIGNED.count();
    if (assigned_count >= MAX_CPU) {
        return INVALID_CPU_NUM;
    }

    auto const cpunum = static_cast<CpuNum>(assigned_count);
    ASSIGNED.set(cpunum, true);
    Cpu::set_state(cpunum, Cpu::State::Possible);
    APICID_TO_CPU[apicid] = cpunum;
    CPU_TO_APICID[cpunum] = apicid;
    return cpunum;
}

auto cpunum_to_apic_id(CpuNum cpunum) -> u32 {
    if (cpunum >= MAX_CPU) {
        return arch::INVALID_APIC_ID;
    }

    auto const apicid = CPU_TO_APICID[cpunum];
    DEBUG_ASSERT(apicid != arch::INVALID_APIC_ID);
    return apicid;
}

auto apic_id_to_cpunum(u32 apicid) -> CpuNum {
    if (apicid >= MAX_CPU) {
        return INVALID_CPU_NUM;
    }

    auto const cpunum = APICID_TO_CPU[apicid];
    DEBUG_ASSERT(cpunum != INVALID_CPU_NUM);
    return cpunum;
}

static arch::XApic XAPIC_STORAGE;
arch::XApic *XAPIC;

auto apic_issue_eoi() -> void {
    XAPIC_STORAGE.issue_eoi();
}

[[maybe_unused]]
irq::Chip XAPIC_CHIP{
    .mask = [](irq::Data const &data) -> void { XAPIC_STORAGE.mask(arch::XApicRegType::LvtLint0); },
    .unmask = [](irq::Data const &data) -> void {
        XAPIC_STORAGE.unmask(arch::XApicRegType::LvtLint0);
    },
    .issue_ack = [](irq::Data const &data) -> void { XAPIC_STORAGE.issue_eoi(); },
    .name = "XApic",
};

/// This is the local APIC timer event device for non-deadline TSC devices.
/// Rated lower than the PIT's 110, and that is fine: it is per-CPU, and
/// TickDevice prefers a per-CPU device over a global one before it looks at
/// ratings at all.
constexpr static auto LAPIC_CLKEVT_RATING = ClockEventDevice::Rating(100);

namespace {
    /// The Timer Divide Configuration Register does not hold the divisor; it
    /// holds a code, and the codes are not in numeric order because bit 2 is
    /// skipped: 0xb is divide-by-1, then 0x0, 0x1, 0x2, 0x3, 0x8, 0x9, 0xa
    /// for 2 through 128. Indexed by log2 of the divisor, which is why every
    /// divisor in this file is carried as its log2.
    constexpr u32 const DIVISOR_ENCODING[] = {0xb, 0x0, 0x1, 0x2, 0x3, 0x8, 0x9, 0xa};
    constexpr auto MAX_DIVISOR_LOG2 = usize(7);
    static_assert(sizeof(DIVISOR_ENCODING) / sizeof(DIVISOR_ENCODING[0]) == MAX_DIVISOR_LOG2 + 1);

    FORCE_INLINE
    auto apic_timer_set_divisor(usize divisor_log2) -> void {
        DEBUG_ASSERT(divisor_log2 <= MAX_DIVISOR_LOG2);
        XAPIC_STORAGE.write_reg(arch::XApicRegType::TimerDivConf, DIVISOR_ENCODING[divisor_log2]);
    }

    FORCE_INLINE
    auto apic_timer_current_count() -> u32 {
        return XAPIC_STORAGE.read_reg(arch::XApicRegType::TimerCurrentCount);
    }
} // namespace

class ApicTimerEvent {
    using Self = ApicTimerEvent;

  public:
    /// The cpumask is left null here and bound in init_apic_timer(). This
    /// object is CPU_LOCAL: the constructor runs once on the template copy,
    /// which is then replicated byte-for-byte into every CPU's unit, so a
    /// pointer taken here to m_cpumask would point into the template from
    /// every replica. Each CPU re-points it at its own copy when it is up.
    ApicTimerEvent()
        : m_canary(this)
        , m_clkevt("LVT", &Self::VTABLE, ClockEventDevice::Feature::Oneshot, LAPIC_CLKEVT_RATING,
                   nullptr)
        , m_cpumask() {}

    ~ApicTimerEvent() = default;

    auto set_mode_oneshot() -> Status {
        m_canary.verify<Self>();
        return setup_timer(APIC_PERIOD, true, false);
    }

    auto set_mode_periodic() -> Status {
        m_canary.verify<Self>();
        return setup_timer(APIC_PERIOD, false, false);
    }

    auto broadcast(CpuMask const &mask) -> void {
        m_canary.verify<Self>();
        apic_send_ipi_multicast(mask, arch::IrqVec::ApicTimer, arch::ApicDeliveryMode::Fixed);
    }

    auto shutdown() -> void {
        m_canary.verify<Self>();
        XAPIC_STORAGE.disable_tsc(arch::IrqVec::ApicTimer);
        XAPIC_STORAGE.write_reg(arch::XApicRegType::TimerInitCount, 0);
    }

    auto set_next_event_tick(Ticks delta) -> Status;

    auto set_next_event_tick_deadline(Ticks delta) -> Status;

    auto shutdown_deadline() -> void;

    INIT_CODE
    static auto select_timer_mode() -> void;

    static auto setup_timer(u32 clocks, bool oneshot, bool masked) -> Status;

    gktl::Canary m_canary;
    ClockEventDevice m_clkevt;

    /// Exactly this CPU: what makes the device per-CPU to TickDevice.
    CpuMask m_cpumask;

    /// The divided APIC clock's frequency, in Hz. This is the primary result
    /// of calibration and what the device is enrolled with. Zero until the
    /// BSP has calibrated.
    ///
    /// Measured as a ratio of two counters read at the same instants -- see
    /// Calibration::hz_of() -- rather than as counts-per-tick multiplied by
    /// HZ. The old way inherited all of the tick's own timing error, because
    /// it assumed a tick was exactly 1/HZ: measured that way the answer moved
    /// 2.4% between two boots of the same machine, and measured as a ratio it
    /// moves 14 ppm.
    inline static u64 APIC_HZ;

    /// Counts of the divided APIC clock in one tick, i.e. APIC_HZ / HZ, which
    /// is what setup_timer() loads for a periodic tick. Derived, not measured.
    inline static u32 APIC_PERIOD;

    /// log2 of the divisor APIC_PERIOD was measured at, and the floor
    /// set_next_event_tick() programs. Carried as a log2 because that is what
    /// indexes DIVISOR_ENCODING, and because every adjustment is a doubling.
    inline static u8 DIVISOR_LOG2;

    /// The TSC's frequency, in Hz, measured over the same window in the same
    /// way. This is the device's tick rate in TSC-deadline mode, where a
    /// "tick" is a TSC cycle rather than a bus cycle. The two counters were
    /// always sampled together; only the TSC half used to be thrown away,
    /// which is why the deadline path had a standing TODO about programming
    /// every deadline off by the ratio between them.
    inline static u64 TSC_HZ;

    /// Which of the two modes this machine uses, decided once by the BSP in
    /// select_timer_mode() before any AP is running or any device enrolled.
    inline static bool USE_TSC_DEADLINE;

    using Packer = ClockEventDevice::Vtable::Packer<&ApicTimerEvent::m_clkevt>;

    /// A *mutable copy* of the generated table, unlike every other driver's,
    /// because this device has two implementations of one slot and picks
    /// between them at run time: select_timer_mode() overwrites two entries
    /// once, on the BSP, before any AP is running or any device is enrolled.
    /// The generated table supplies the counting-mode defaults.
    static ClockEventDevice::Vtable VTABLE;
};

ClockEventDevice::Vtable ApicTimerEvent::VTABLE =
    time::CLKEVT_VTABLE_OF<ApicTimerEvent, &ApicTimerEvent::m_clkevt>;

/// Programs the local APIC timer to interrupt |delta| device ticks from now.
///
/// The timer counts a 32-bit initial count down to zero at (bus frequency /
/// divisor), so the divisor is what buys range: a delta that does not fit in
/// 32 bits is served by slowing the counter and shifting the count down, one
/// bit of range per doubling, trading resolution for reach. DIVISOR_LOG2 is
/// what the calibration settled on and is the floor.
auto ApicTimerEvent::set_next_event_tick(Ticks delta) -> Status {
    m_canary.verify<Self>();

    auto const highest_set_bit = delta < 2 ? usize(0) : usize(ustl::bit_width(delta) - 1);
    auto divisor_log2 = usize(DIVISOR_LOG2);
    usize count = delta;

    if (highest_set_bit > 31) {
        auto const extra_bits = highest_set_bit - 31;
        divisor_log2 += extra_bits;

        if (divisor_log2 > MAX_DIVISOR_LOG2) {
            // Further away than the slowest tick can reach. Run the counter
            // to its end instead of refusing: waking early costs one pass
            // over a timer queue that finds nothing due and reprograms,
            // whereas refusing leaves the CPU with no pending interrupt.
            divisor_log2 = MAX_DIVISOR_LOG2;
            count = ustl::NumericLimits<u32>::max();
        } else {
            count >>= extra_bits;
            if (!count) {
                count = 1;
            }
        }
    }

    // Divide configuration first. Writing the initial count is what starts
    // the countdown, so a divisor written after it would not govern the
    // interval just armed.
    apic_timer_set_divisor(divisor_log2);
    XAPIC_STORAGE.write_reg(arch::XApicRegType::TimerInitCount, u32(count));
    return Status::Ok;
}

/// The TSC-deadline variant: there is no counter and no divisor, just an MSR
/// holding an absolute TSC value.
///
/// |delta| arrives in the device's own ticks, and in this mode a tick is a
/// TSC cycle, so the device must have been enrolled with the TSC's rate --
/// see init_apic_timer(), which picks TSC_PERIOD over APIC_PERIOD exactly
/// when this function is the one in the vtable. Before TSC_PERIOD was
/// measured the device was enrolled with the bus rate either way, and every
/// deadline programmed here was off by the ratio between the two.
auto ApicTimerEvent::set_next_event_tick_deadline(Ticks delta) -> Status {
    m_canary.verify<Self>();

    // There was a `delta < 0` rejection here. Ticks is unsigned, so it never
    // fired; a deadline in the past reaches ClockEventDevice::program_event()
    // as a clamp to m_min_delta long before it gets here.
    auto const now = arch::Tick::get();
    XAPIC_STORAGE.set_tsc_deadline(now.tsc + delta);
    return Status::Ok;
}

auto ApicTimerEvent::shutdown_deadline() -> void {
    m_canary.verify<Self>();
    // Disable the TSC deadline timer.
    XAPIC_STORAGE.disable_tsc(arch::IrqVec::ApicTimer);
    XAPIC_STORAGE.write_reg(arch::XApicRegType::TimerInitCount, 0);
}

/// Picks the counting mode or the TSC-deadline mode, once, and points the
/// shared vtable at the matching pair of methods.
///
/// Called by the BSP from apic_calibrate_clock(), before any AP is running
/// and before any ClockEventDevice is enrolled, which is what makes writing
/// VTABLE here safe. It used to be done inside setup_timer(), which every
/// mode switch on every CPU calls at any time -- rewriting function pointers
/// that other CPUs are dispatching through, with no synchronisation, and
/// redoing a once-only decision on every call.
INIT_CODE
auto ApicTimerEvent::select_timer_mode() -> void {
    USE_TSC_DEADLINE = x86_has_feature(arch::CpuFeatureType::TscDeadlineTimer);
    if (!USE_TSC_DEADLINE) {
        return;
    }

    VTABLE.set_next_event_tick = Packer::pack<&Self::set_next_event_tick_deadline>();
    VTABLE.shutdown = Packer::pack<&Self::shutdown_deadline>();
}

auto ApicTimerEvent::setup_timer(u32 clocks, bool oneshot, bool masked) -> Status {
    using namespace arch;

    if (USE_TSC_DEADLINE) {
        XAPIC_STORAGE.enable_tsc(IrqVec::ApicTimer, XApic::TscMode::Deadline);
    } else if (oneshot) {
        XAPIC_STORAGE.enable_tsc(IrqVec::ApicTimer, XApic::TscMode::OneShot);
    } else {
        XAPIC_STORAGE.enable_tsc(IrqVec::ApicTimer, XApic::TscMode::Periodic);
    }

    if (masked) {
        XAPIC_STORAGE.mask(arch::XApicRegType::LvtTimer);
    } else {
        XAPIC_STORAGE.unmask(arch::XApicRegType::LvtTimer);
    }

    // Ignored in TSC-deadline mode, where the deadline MSR is the whole
    // interface; harmless there and required in the counting modes.
    apic_timer_set_divisor(DIVISOR_LOG2);
    XAPIC_STORAGE.write_reg(arch::XApicRegType::TimerInitCount, clocks);
    return Status::Ok;
}

CPU_LOCAL
static ApicTimerEvent CLKDEV_STORAGE;

/// The ApicTimer vector's entry. Every CPU's local APIC timer raises the same
/// vector, but each one only ever interrupts its own CPU, so the device that
/// fired is always the calling CPU's copy of CLKDEV_STORAGE, and the handler
/// takes that copy rather than any shared pointer. Resolving "this CPU" is safe
/// here without pinning: this runs with interrupts off, so nothing can migrate
/// the caller between reading the per-CPU offset and using what it names.
///
/// It used to go through a plain global `ClockEventDevice *` that every CPU's
/// init_apic_timer() overwrote, so after bring-up every CPU's interrupt
/// handled the *last* AP's device. The device is oneshot-only and TickDevice
/// emulates the periodic tick by re-arming from the device's own
/// next_event(), so N CPUs took turns pushing one shared deadline a period
/// further out, and each of them ticked at HZ / N.
auto apic_handle_timer_irq() -> void {
    auto &self = CpuLocal::access(CLKDEV_STORAGE);
    // The per-CPU form of the null check that used to guard the global: the
    // LVT timer is masked out of reset and init_apic_timer() leaves it masked,
    // so this vector cannot fire before init_apic_timer() has bound the device
    // to this CPU. The template's m_cpumask is empty, so an unbound copy fails.
    DEBUG_ASSERT(self.m_cpumask.test(Cpu::id()),
                 "APIC timer interrupt on CPU[{}] before its device was bound", Cpu::id());

    apic_issue_eoi();
    self.m_clkevt.handle_event();
}

namespace {
    /// The two counters that matter, sampled at both ends of a window.
    struct Calibration {
        auto start() -> void {
            apic_first = apic_timer_current_count();
            tsc_first = arch::Tick::get().tsc;
            mono_first = time::SteadyClock::now();
        }

        auto finish() -> void {
            apic_last = apic_timer_current_count();
            tsc_last = arch::Tick::get().tsc;
            mono_last = time::SteadyClock::now();
        }

        /// The APIC timer counts *down* from its initial count (SDM vol. 3
        /// section 11.5.4), so the first sample is the larger one. This used
        /// to be computed the other way round, which after the unsigned wrap
        /// and the truncation to 32 bits yielded `2^32 - elapsed` -- a number
        /// that then went through a subtraction of two such values and wrapped
        /// again. Nothing downstream of it meant anything.
        [[nodiscard]]
        auto apic_elapsed() const -> u32 {
            return apic_first - apic_last;
        }

        /// The TSC counts up, so this one is the natural way round.
        [[nodiscard]]
        auto tsc_elapsed() const -> u64 {
            return tsc_last - tsc_first;
        }

        /// The same window as read by the steady clock, i.e. by whatever
        /// hardware counter has been enrolled as the clock source -- the HPET
        /// here. This is the independent yardstick the two counters above are
        /// measured against; it shares no hardware with either, and its own
        /// scale comes from a hardware constant (the HPET's advertised
        /// period) rather than from any measurement.
        [[nodiscard]]
        auto mono_elapsed() const -> Duration {
            return mono_last - mono_first;
        }

        /// True when the one-shot counter reached zero inside the window, so
        /// its elapsed count is a floor rather than a measurement.
        [[nodiscard]]
        auto counter_ran_out() const -> bool {
            return apic_last == 0;
        }

        /// Turns a count over this window into a frequency in Hz.
        ///
        /// This is the whole point of sampling the clock source alongside the
        /// counters: |count| and mono_elapsed() were read at the same two
        /// instants, so however late those reads were, they were late
        /// together and the lateness divides out. The window's length never
        /// enters the answer and does not have to be known -- which means the
        /// result does not depend on the tick being accurate, only on the
        /// clock source's scale, which is a hardware constant.
        ///
        /// That independence is measurable: across two boots the counts
        /// themselves differed by 2.4%, while the frequencies this returns
        /// differed by 14 ppm.
        ///
        /// Returns zero when there is no clock source to divide by, which the
        /// caller treats as "fall back to counting ticks".
        [[nodiscard]]
        auto hz_of(u64 count) const -> u64 {
            constexpr auto NS_PER_SEC = u64(duration_cast<Duration>(1_s).count());

            auto const elapsed_ns = u64(mono_elapsed().count());
            if (elapsed_ns == 0) {
                return 0;
            }
            return count * NS_PER_SEC / elapsed_ns;
        }

        u32 apic_first = 0;
        u32 apic_last = 0;
        u64 tsc_first = 0;
        u64 tsc_last = 0;
        TimePoint<> mono_first{};
        TimePoint<> mono_last{};
    };

    /// The tick is the only thing that moves jiffies, and until it is proven
    /// to be running there is no clock to time a wait against -- so the bound
    /// has to be in iterations. A billion loads is on the order of a second
    /// on any CPU this kernel runs on: far longer than the 10 ms a tick
    /// should take, and short enough that a dead tick is reported instead of
    /// hanging the boot, which is what the unbounded loop here used to do.
    constexpr auto MAX_TICK_WAIT_SPINS = usize(1000'000'000);

    /// Waits until jiffies has advanced |count| past |from|. False if it
    /// never did.
    INIT_CODE
    auto wait_for_jiffies(Ticks from, Ticks count) -> bool {
        for (auto spins = usize(0); spins < MAX_TICK_WAIT_SPINS; ++spins) {
            if (ClockSource::Default::read() - from >= count) {
                return true;
            }
            arch::spin_hint();
        }
        return false;
    }

    /// Measures the APIC timer and the TSC across |jiffies| ticks.
    ///
    /// Two things about the window. It is aligned to a jiffy edge first:
    /// without that it would start at a random phase inside the current tick,
    /// so its true length would be anywhere in ((jiffies-1), jiffies] ticks --
    /// up to a whole 10 ms of error on a 10 ms measurement.
    ///
    /// And the timer is put into a *counting* mode by hand rather than
    /// through setup_timer(). setup_timer() selects TSC-deadline whenever the
    /// CPU offers it, and in that mode the initial-count register is ignored
    /// and the current-count register reads zero -- so calibration was
    /// measuring a counter that was not running, got an elapsed count of
    /// zero, and left APIC_PERIOD at zero. On every machine newer than Sandy
    /// Bridge that meant the local APIC timer was never enrolled at all, and
    /// every CPU but one was left without a tick.
    INIT_CODE
    auto calibrate_once(Ticks jiffies, Calibration *out) -> bool {
        constexpr auto APIC_INIT_COUNT = ustl::NumericLimits<u32>::max();

        XAPIC_STORAGE.enable_tsc(arch::IrqVec::ApicTimer, arch::XApic::TscMode::OneShot);
        XAPIC_STORAGE.mask(arch::XApicRegType::LvtTimer);
        apic_timer_set_divisor(ApicTimerEvent::DIVISOR_LOG2);

        auto const edge = ClockSource::Default::read();
        if (!wait_for_jiffies(edge, 1)) {
            return false;
        }

        // Started only now, on the edge, so the count does not include the
        // ragged remainder of the tick we arrived in.
        XAPIC_STORAGE.write_reg(arch::XApicRegType::TimerInitCount, APIC_INIT_COUNT);
        auto const start = ClockSource::Default::read();
        out->start();

        if (!wait_for_jiffies(start, jiffies)) {
            return false;
        }
        out->finish();

        return true;
    }
} // namespace

/// Measures how fast the local APIC timer and the TSC run, in units of the
/// tick, and records both.
///
/// Two windows of different lengths are measured and subtracted. The
/// difference cancels whatever fixed cost sits at each end of a window --
/// reading the registers, entering and leaving the wait -- because that cost
/// is the same for both. What it cannot cancel is a *random* error, which is
/// why calibrate_once() aligns to a tick edge rather than relying on the
/// subtraction to hide the phase.
///
/// The two windows are the shortest and the longest, not the shortest two:
/// the accuracy of the difference is set by how far apart they are. The old
/// code measured four windows, then computed its answer from windows 0 and 1
/// and never read the other two.
INIT_CODE
auto apic_calibrate_clock() -> bool {
    // Divisor 1: the finest resolution the timer offers. At 2^32 counts it
    // spans four seconds at a gigahertz, comfortably more than the window
    // below -- but not unboundedly, which is why the overflow check exists.
    // The old code passed an "advisor" divisor down to the measurement, which
    // ignored it, and then recorded it as the one in force, so the count was
    // taken at the reset default of 2 and written down as 1.
    ApicTimerEvent::DIVISOR_LOG2 = 0;

    // 200 ms, up from 70 ms. The endpoint error is a fixed cost, independent
    // of how long the window is, so the *relative* error falls as 1/T: three
    // times the span is a third of the error, for 130 ms more boot time.
    constexpr auto SHORT_JIFFIES = Ticks(1);
    constexpr auto LONG_JIFFIES = Ticks(20);

    DEBUG_ASSERT(!arch::irqs_disabled(), "The tick cannot advance with interrupts off");

    Calibration shortwin;
    Calibration longwin;
    if (!calibrate_once(SHORT_JIFFIES, &shortwin) || !calibrate_once(LONG_JIFFIES, &longwin)) {
        log::error("APIC calibration: the periodic tick is not running");
        return false;
    }

    // The timer runs one-shot from 0xFFFFFFFF and stops dead at zero, so a
    // window longer than the counter can span reads as "everything elapsed"
    // and the frequency comes out as whatever the window happened to be.
    // Reaching zero is the only observable symptom.
    if (longwin.counter_ran_out()) {
        log::error("APIC calibration: the timer counted out before the window ended");
        return false;
    }

    if (longwin.apic_elapsed() <= shortwin.apic_elapsed() ||
        longwin.tsc_elapsed() <= shortwin.tsc_elapsed()) {
        log::error("APIC calibration: counters did not advance ({} / {} APIC, {} / {} TSC)",
                   shortwin.apic_elapsed(), longwin.apic_elapsed(), shortwin.tsc_elapsed(),
                   longwin.tsc_elapsed());
        return false;
    }

    // The tick period, from the two windows. This one *does* need the
    // subtraction: the ruler here is a count of whole ticks, so the fixed
    // endpoint cost does not divide out of a single window the way it does
    // out of a ratio -- it has to be cancelled between two windows of
    // different length.
    constexpr auto SPAN = u32(LONG_JIFFIES - SHORT_JIFFIES);
    auto const mono_span = longwin.mono_elapsed() - shortwin.mono_elapsed();

    // The frequencies, from the long window alone. No subtraction, because
    // Calibration::hz_of() divides two counters read at the same instants and
    // the endpoint cost is already common to both.
    auto apic_hz = longwin.hz_of(longwin.apic_elapsed());
    auto tsc_hz = longwin.hz_of(longwin.tsc_elapsed());

    if (mono_span == Duration::zero() || apic_hz == 0) {
        // No clock source enrolled yet, so there is no independent ruler:
        // fall back to counting ticks and trusting that a tick is 1/HZ. Less
        // accurate and unverifiable, but it is what the machine has.
        log::warn("APIC calibration: no clock source to measure against, counting ticks instead");

        apic_hz = u64((longwin.apic_elapsed() - shortwin.apic_elapsed()) / SPAN) * u64(time::HZ);
        tsc_hz = ((longwin.tsc_elapsed() - shortwin.tsc_elapsed()) / SPAN) * u64(time::HZ);
    } else {
        // Cross-check the tick device against the clock source. Two
        // thresholds, because the errors worth catching and the noise worth
        // tolerating are an order of magnitude apart: everything that can go
        // wrong with a tick device is a *factor* (a PIT reload left at 65536
        // makes the tick 5.5x too long, a doubly-counted tick makes it half,
        // a stopped device times out), while measurement noise is a few
        // percent -- under TCG, where the virtual clock follows host
        // scheduling, successive boots here differed by 7%. So warn at 10%,
        // and only refuse at 2x.
        auto const measured_tick = mono_span / SPAN;
        auto const expected_tick = time::NSEC_PER_TICK;
        auto const drift = measured_tick > expected_tick ? measured_tick - expected_tick
                                                         : expected_tick - measured_tick;

        log::info("tick period: {} ns measured by the clock source, {} ns expected",
                  measured_tick.count(), expected_tick.count());

        if (drift * 10 > expected_tick) {
            log::warn("tick period is {}% off what the clock source says",
                      (drift * 100) / expected_tick);
        }
        if (drift > expected_tick) {
            log::error("APIC calibration: the tick device and the clock source disagree by "
                       "more than a factor of two; the tick is misprogrammed");
            return false;
        }
    }

    if (apic_hz == 0 || tsc_hz == 0) {
        log::error("APIC calibration: measured a zero frequency");
        return false;
    }

    ApicTimerEvent::APIC_HZ = apic_hz;
    ApicTimerEvent::TSC_HZ = tsc_hz;
    ApicTimerEvent::APIC_PERIOD = u32(apic_hz / u64(time::HZ));
    log::info("APIC timer: {} Hz at divisor 1, TSC {} Hz", apic_hz, tsc_hz);

    // Safe here and nowhere later: still on the BSP, no AP is running, and no
    // ClockEventDevice has been enrolled, so nothing can be dispatching
    // through the vtable this rewrites.
    ApicTimerEvent::select_timer_mode();
    return true;
}

/// Written once, by the BSP in apic_calibrate_clock(), before any AP runs
/// and before anything that reads it exists, so it needs no atomic.
auto apic_tsc_hz() -> u64 {
    return ApicTimerEvent::TSC_HZ;
}

INIT_CODE
auto current_apic_id_early() -> u32 {
    return XAPIC_STORAGE.id();
}

INIT_CODE
static auto init_apic_timer() -> void {
    using namespace arch;

    // By default to let timer work on periodic mode.
    XAPIC_STORAGE.enable_tsc(IrqVec::ApicTimer, XApic::TscMode::Periodic);
    XAPIC_STORAGE.mask(XApicRegType::LvtTimer);

    auto &self = CpuLocal::access(CLKDEV_STORAGE);
    self.m_cpumask = CpuMask::of(Cpu::id());
    self.m_clkevt.set_cpumask(&self.m_cpumask);

    // A "tick" of this device is whatever set_next_event_tick() counts, and
    // that depends on the mode: a divided bus cycle in the counting mode, a
    // TSC cycle in TSC-deadline mode. Enrol with the rate of whichever one is
    // in the vtable.
    //
    // Nothing is enrolled until the BSP has calibrated. Enrolling with a
    // made-up rate would hand the tick layer a device whose every delta is
    // wrong, which is worse than staying on the PIT/HPET a little longer.
    // APs only reach here after the BSP, so for them this is never zero.
    //
    // The bounds: 0xF because the counter cannot reliably deliver a shorter
    // countdown, 0x7FFFFFFF because set_next_event_tick() spills anything wider
    // into the divisor.
    auto const freq =
        ApicTimerEvent::USE_TSC_DEADLINE ? ApicTimerEvent::TSC_HZ : ApicTimerEvent::APIC_HZ;
    if (freq == 0) {
        log::info("APIC timer not calibrated on CPU[{}], skip enrolling it", Cpu::id());
        return;
    }
    ClockEventDevice::enroll(self.m_clkevt, usize(freq), 0xF, 0x7FFFFFFF);
}

/// Initialize APIC performance monitoring interrupt.
INIT_CODE
static auto init_apic_pmi() -> void {
    using namespace arch;
    XAPIC_STORAGE.write_reg(XApicRegType::LvtPmi, IrqVec::ApicPmi);
    XAPIC_STORAGE.mask(arch::XApicRegType::LvtPmi);
}

INIT_CODE
static auto init_apic_error() -> void {
    using namespace arch;
    XAPIC_STORAGE.write_reg(XApicRegType::LvtError, IrqVec::ApicError);
    XAPIC_STORAGE.write_reg(XApicRegType::Esr, 0);
}

/// Masks LINT0, the 8259's virtual wire, on every CPU.
///
/// The firmware leaves the boot CPU's LINT0 in ExtINT mode and unmasked,
/// the wire through which the legacy 8259 interrupt controller reaches that
/// CPU alone. Nothing the kernel wants comes that way: the platform masks
/// the 8259 at its own registers and routes every device interrupt through
/// the IO-APIC (platform_init_apic(), which calls this on the boot CPU
/// before it turns interrupts on). Left open, the wire still costs the boot
/// CPU: an emulator re-evaluates the 8259's output whenever any legacy line
/// is driven -- every byte any CPU writes to the UART -- and interrupts the
/// boot CPU to look at it, which under QEMU's TCG made the boot CPU run
/// many times slower for as long as other CPUs logged. An AP comes out of
/// INIT with LINT0 masked already; it is written all the same, so that no
/// CPU depends on what it was left.
///
/// What the firmware left in the boot CPU's LINT0 is logged first, as for
/// LINT1.
INIT_CODE
static auto init_apic_lint0(bool const is_boot_cpu) -> void {
    if (is_boot_cpu) {
        log::info("local APIC: the firmware left LINT0 at {:#x}; masking it, as nothing uses the "
                  "8259's virtual wire",
                  XAPIC_STORAGE.read_reg(arch::XApicRegType::LvtLint0));
    }
    XAPIC_STORAGE.mask_lint0();
}

/// Points LINT1 at NMI delivery, unmasked on the boot CPU only.
///
/// An NMI from outside the machine -- QEMU's monitor `nmi`, a front-panel
/// button -- comes in on LINT1, and the lockup detector takes one nobody in
/// the kernel sent as a request to dump every CPU: it is how a machine whose
/// every CPU has interrupts off can still be made to report. One CPU is
/// enough to take it, and the dump then reaches the others itself, so the
/// rest keep LINT1 masked, which is how they come out of INIT.
///
/// What the firmware left in the boot CPU's LINT1 is logged first, because
/// it says whether such an NMI reached the kernel before it set LINT1 itself.
INIT_CODE
static auto init_apic_lint1(bool const is_boot_cpu) -> void {
    if (is_boot_cpu) {
        log::info("local APIC: the firmware left LINT1 at {:#x}; routing it to NMI",
                  XAPIC_STORAGE.read_reg(arch::XApicRegType::LvtLint1));
    }
    XAPIC_STORAGE.route_lint1_to_nmi(!is_boot_cpu);
}

INIT_CODE
auto init_local_apic_percpu() -> void {
    using namespace arch;
    auto apic_base = MsrIo::read<usize>(MsrRegAddr::IA32ApicBase);
    if (apic_base & arch::IA32_APIC_BASE_BSP) {
        if (x86_has_feature(CpuFeatureType::X2Apic)) {
            log::trace("X2-APIC enabled");
        }
    }

    apic_base |= arch::IA32_APIC_BASE_XAPIC_ENABLE;
    if (X2APIC_ENABLED) {
        apic_base |= arch::IA32_APIC_BASE_X2_APIC_ENABLE;
    }

    // Enable X[2]-APIC
    MsrIo::write<usize>(MsrRegAddr::IA32ApicBase, apic_base);

    // Enabling the APIC through IA32_APIC_BASE above is only half of it: that
    // bit is the *hardware* enable, and the local APIC still delivers nothing
    // until SVR bit 8, the software enable, is set. A CPU leaves INIT/SIPI
    // with SVR = 0xFF -- software-disabled -- so every AP came up deaf: the
    // reschedule IPI that x86_bring_aps() and the scheduler send was accepted
    // by nothing and the CPU stayed in `sti; hlt` forever with a runnable
    // thread on its queue. The boot CPU only worked by luck, because the
    // firmware had already enabled its APIC (SVR = 0x10F) before the kernel
    // ran; this also replaces the spurious vector the firmware chose (0x0F)
    // with the one this kernel actually has a handler for.
    XAPIC_STORAGE.enable(IrqVec::ApicSpurious);

    // Save ID of this local APIC
    CpuLocal::write(APIC_ID, XAPIC_STORAGE.id());

    if (!(apic_base & arch::IA32_APIC_BASE_BSP)) {
        init_apic_timer();
    }
    init_apic_error();
    init_apic_pmi();
    init_apic_lint0((apic_base & arch::IA32_APIC_BASE_BSP) != 0);
    init_apic_lint1((apic_base & arch::IA32_APIC_BASE_BSP) != 0);
}

INIT_CODE
auto init_local_apic() -> void {
    // Section 10.4.1 has the following:
    // In MP system configurations, the APIC registers for Intel 64 or IA-32 processors on
    // the system bus are initially mapped to the same 4-KByte region of the physical address
    // space. Software has the option of changing initial mapping to a different 4-KByte region
    // for all the local APICs or of mapping the APIC registers for each local APIC to its own
    // 4-KByte region.
    //
    // We map it to the same 4K region for all the local APICs.
    auto result = mm::VmAspace::kernel_aspace()->root_vma()->map_phys(
        arch::APIC_PHYS_BASE, mm::VmPlacement::anywhere(), PAGE_SIZE,
        // `Discache` is required to prevent
        mm::MmuFlags::Discache | mm::MmuFlags::Readable | mm::MmuFlags::Writable,
        mm::VmMapOption::Populate | mm::VmMapOption::Pinned, "Local-APIC");
    DEBUG_ASSERT(result, "Failed to allocate MMIO for BP's Local APIC");

    auto mmio_base(reinterpret_cast<volatile u32 *>((*result)->base()));

    XAPIC_STORAGE.init(mmio_base);

    init_local_apic_percpu();
    XAPIC = &XAPIC_STORAGE;
}

static auto init_bsp_apic_timer() -> void {
    DEBUG_ASSERT(!arch::irqs_disabled());
    // On BSP, we need to calibrate the APIC clock.
    if (!apic_calibrate_clock()) {
        return;
    }

    init_apic_timer();
}
GKTL_INIT_HOOK(InitBspApicTimer, init_bsp_apic_timer, gktl::InitLevel::Irq + 4);

} // namespace ours
