#include <arch/spin_hint.hpp>
#include <gktl/canary.hpp>
#include <ktl/new.hpp>
#include <logz4/log.hpp>
#include <ours/arch/apic.hpp>
#include <ours/arch/hpet.hpp>
#include <ours/arch/hpet_regs.hpp>
#include <ours/arch/time.hpp>
#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/platform/isa.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock_event_device.hpp>
#include <ours/time/clock_source.hpp>
#include <ustl/chrono/duration.hpp>
#include <ustl/traits/cv.hpp>
#include <ustl/views/span.hpp>

using namespace ustl::chrono;

namespace ours {
using time::ClockEventDevice;
using time::ClockSource;

constexpr static auto HPET_MIN_CYCLES = 128;

/// Deliberately above the PIT's 110, where Linux puts the HPET at 50.
///
/// In Linux the two never compete: hpet_time_init() tries the HPET first and
/// only registers the PIT if that fails, so the HPET's low rating is never
/// tested against it. Here the PIT is enrolled first (arch_time_init(), at
/// Irq + 1) and the HPET after (Irq + 2), and enabling the legacy replacement
/// route below takes IRQ0 away from the PIT -- from then on the HPET is the
/// device on that line, and time::CLOCK_EVENT is pointed at it. The device
/// that owns the interrupt has to be the one TickDevice is bound to, so the
/// HPET must win the comparison it would lose on Linux's numbers.
constexpr static auto HPET_CLKEVT_RATING = ClockEventDevice::Rating(120);

/// As a *time source* the HPET keeps Linux's number (drivers/clocksource has
/// it at 250, between the ACPI PM timer's 200 and the TSC's 300). Unlike the
/// clock-event rating above there is no ordering to fight here: nothing else
/// is enrolled as a clock source yet.
constexpr static auto HPET_CLKSRC_RATING = ClockSource::Rating(250);

class HpetChannel;

static arch::HpetRegs *HPET_MMIO;
static usize HPET_FREQUENCY;
static ustl::views::Span<HpetChannel> HPET_CHANNELS;
[[maybe_unused]]
static bool HPET_LEGACY_IRQ_ENABLED;

class HpetClockSource {
    using Self = HpetClockSource;

  public:
    HpetClockSource()
        : m_canary(this), m_spinlock(), m_clksrc("HPET-CLKSRC", &VTABLE), m_cache(0) {}

    auto read() -> u64 {
        m_canary.verify<Self>();

        u64 const oldvalue = m_cache;
        if (::arch::AutoInterruptDisabler guard; m_spinlock.try_acquire()) {
            auto const value = HPET_MMIO->main_counter_value;
            m_cache = value;
            m_spinlock.release();
            return value;
        }

        // In contend case, threads except first thread all read
        // the cache value of clock cycle recorded by first thread.
        u64 newvalue;
        do {
            newvalue = m_cache;
        } while (oldvalue == newvalue && m_spinlock.is_locked());

        return newvalue;
    }

    auto suspend() -> Status {
        m_canary.verify<Self>();
        HPET_MMIO->general_config.disable_cnf();
        return Status::Ok;
    }

    auto resume() -> Status {
        m_canary.verify<Self>();
        HPET_MMIO->general_config.enable_cnf();
        return Status::Ok;
    }

    auto high_level_source() -> ClockSource & {
        return m_clksrc;
    }

  private:
    gktl::Canary m_canary;
    sync::SpinLock m_spinlock;
    ClockSource m_clksrc;
    usize m_cache;

    using Packer = ClockSource::Vtable::Packer<&Self::m_clksrc>;
    constexpr static ClockSource::Vtable VTABLE{
        .read = Packer::pack<&Self::read>(),
        .enable = nullptr,
        .disable = nullptr,
        .suspend = Packer::pack<&Self::suspend>(),
        .resume = Packer::pack<&Self::resume>(),
    };
};
static HpetClockSource HPET_CLOCK_SOURCE;

class HpetChannel {
    using Self = HpetChannel;

  public:
    HpetChannel()
        : m_canary(this)
        , id_(0)
        , regs_mmio_(nullptr)
        , m_clkevt("HPET", &time::CLKEVT_VTABLE_OF<Self, &Self::m_clkevt>,
                   ClockEventDevice::Feature::Periodic | ClockEventDevice::Feature::Oneshot,
                   HPET_CLKEVT_RATING, nullptr) {}

    ~HpetChannel() = default;

    auto init(usize id, arch::HpetTimerRegs *mmio) -> void {
        m_canary.verify<Self>();
        id_ = id;
        regs_mmio_ = mmio;
        m_clkevt.set_irqnum(static_cast<VIrqNum>(mmio->conf_caps.irq()));
    }

    /// Arms the comparator |delta| ticks ahead of the main counter, and
    /// reports whether that was in time. Linux's hpet_next_event().
    ///
    /// The read-back is not optional. An HPET comparator fires on *equality*
    /// with the main counter -- there is no "less than or equal", and no
    /// count-down mode -- so a comparator written even one tick late is not
    /// late, it is missed entirely, and the next interrupt comes when the
    /// counter wraps, which at 14 MHz on 64 bits is never. Between reading
    /// the counter and writing the comparator sit an SMI, an NMI, and on
    /// several chipsets an internal write delay of a couple of HPET cycles,
    /// so this is a real window and not a theoretical one.
    ///
    /// The verdict is Linux's: not just "did we already pass it" but "are we
    /// within HPET_MIN_CYCLES of it", because the write may still be in
    /// flight inside the chipset. Status::TimeOut is -ETIME; the caller
    /// (ClockEventDevice::program_event) retries further out.
    auto set_next_event_tick(Ticks delta) -> Status {
        m_canary.verify<Self>();

        auto const target = HPET_MMIO->main_counter_value + delta;
        regs_mmio_->comparator_value = target;

        auto const remaining = static_cast<i64>(target - HPET_MMIO->main_counter_value);
        if (remaining < HPET_MIN_CYCLES) {
            return Status::TimeOut;
        }
        return Status::Ok;
    }

    auto set_mode_oneshot() -> Status {
        m_canary.verify<Self>();
        regs_mmio_->conf_caps.set_mode_oneshot().mask(false);
        return Status::Ok;
    }

    /// Arms this channel to interrupt every 1/HZ, and keeps it doing so.
    ///
    /// The comparator is written twice and the two writes mean different
    /// things. With Tn_VAL_SET_CNF set, the first write lands in the
    /// comparator -- the absolute counter value of the first interrupt -- and
    /// clears Tn_VAL_SET_CNF along the way; the second lands in the period
    /// accumulator, which is what the comparator advances by from then on.
    ///
    /// This used to be a single write, made by hand in hpet_init_internal()
    /// rather than here, so the accumulator stayed at zero. An HPET
    /// comparator matches on equality, so one that never advances is matched
    /// exactly once and then never again: the tick fired a single time and
    /// stopped. Since the HPET displaces the PIT as the tick device, that
    /// froze jiffies, and the local APIC calibration -- which spins waiting
    /// for jiffies -- hung the boot there. Linux's hpet_set_periodic() writes
    /// twice for exactly this reason.
    auto set_mode_periodic() -> Status {
        m_canary.verify<Self>();
        if (!regs_mmio_->conf_caps.support_periodic()) {
            return Status::Unsupported;
        }

        auto const period = HPET_FREQUENCY / usize(time::HZ);
        DEBUG_ASSERT(period > 0, "HPET is slower than the tick rate");

        regs_mmio_->conf_caps.set_mode_periodic().mask(false);
        regs_mmio_->comparator_value = HPET_MMIO->main_counter_value + period;
        regs_mmio_->comparator_value = period;
        return Status::Ok;
    }

    /// Stops this channel. Needed for TickDevice::rebind_device(), which
    /// detaches the outgoing device -- without it the HPET keeps interrupting
    /// on a line whose handler has been moved elsewhere.
    auto shutdown() -> void {
        m_canary.verify<Self>();
        regs_mmio_->conf_caps.mask();
    }

    auto resume() -> Status {
        m_canary.verify<Self>();
        regs_mmio_->conf_caps.mask(false);
        return Status::Ok;
    }

    FORCE_INLINE auto device() -> ClockEventDevice & {
        return m_clkevt;
    }

  private:
    gktl::Canary m_canary;
    usize id_;
    arch::HpetTimerRegs *regs_mmio_;
    // No set_next_event_time: this device is enrolled without
    // Feature::TimePoint, so program_event() never takes that path. Not
    // defining it is how that is now said.
    ClockEventDevice m_clkevt;
};

/// Verifies the main counter is actually running.
///
/// Linux does the same before it registers the HPET as a clocksource, and it
/// matters more here: the HPET is currently the *only* clock source in the
/// tree, so a counter stuck at a constant makes SteadyClock::now() constant
/// too, and then every timer deadline sits forever in the future and nothing
/// ever expires. A million spins is orders of magnitude longer than the ~70 ns
/// a 14 MHz counter needs to move.
INIT_CODE static auto hpet_counter_is_running() -> bool {
    constexpr auto MAX_SPINS = usize(1000'000);

    auto const start = HPET_MMIO->main_counter_value;
    for (auto spins = usize(0); spins < MAX_SPINS; ++spins) {
        if (HPET_MMIO->main_counter_value != start) {
            return true;
        }
        ::arch::spin_hint();
    }

    return false;
}

INIT_CODE static auto hpet_init_internal() -> Status {
    DEBUG_ASSERT(HPET_MMIO);
    auto const mmio = HPET_MMIO;

    if (!mmio->general_caps.support_64bit()) {
        log::info("HPET: Unsupport 64-bit.");
        return Status::Unsupported;
    }

    // Only channel 0 is used, so one is the real requirement. This used to
    // demand two, with no stated reason -- a made-up requirement that can
    // reject a working device is worse than none. (num_timers() already
    // undoes the specification's minus-one encoding of NUM_TIM_CAP.)
    auto const num_channels = mmio->general_caps.num_timers();
    if (num_channels < 1) {
        log::info("HPET: No timer channels.");
        return Status::NoResource;
    }

    auto const channels = new (mm::GAF_KERNEL) HpetChannel[num_channels];
    if (!channels) {
        return Status::OutOfMem;
    }

    HPET_CHANNELS = ustl::views::make_span(channels, num_channels);
    for (auto i = 0; i < num_channels; ++i) {
        HPET_CHANNELS[i].init(i, &mmio->timers[i]);
    }

    // The counter period, in femtoseconds. Bounded the way Linux bounds it:
    // below 1e5 fs the implied frequency would exceed 10 GHz and above 1e8 fs
    // it would fall below 10 MHz, neither of which an HPET is. Only a zero
    // was rejected before, so firmware filling in a wild value would have set
    // HPET_FREQUENCY -- and that is the conversion basis for both the clock
    // source and the clock event device, so everything downstream would have
    // inherited it.
    constexpr auto MIN_PERIOD = u32(100'000);
    constexpr auto MAX_PERIOD = u32(100'000'000);

    constexpr auto FEMTOSECONDS_PER_SECOND = duration_cast<Femtoseconds>(Seconds(1)).count();
    auto const period = mmio->general_caps.period();
    if (period < MIN_PERIOD || period > MAX_PERIOD) {
        log::info("HPET: period {} fs is out of range.", period);
        return Status::InvalidArguments;
    }

    // HZ: How many ticks in one second.
    HPET_FREQUENCY = (FEMTOSECONDS_PER_SECOND + period - 1) / period;
    log::info("HPET Frequency = {}", HPET_FREQUENCY);

    // Route channel 0 onto the legacy IRQ0 line and start the counter. What
    // is deliberately *not* done here any more is programming channel 0's
    // comparator: that is HpetChannel::set_mode_periodic()'s job, reached
    // through ClockEventDevice::enroll() a few lines below. Doing it in both
    // places gave the device two owners, and the hand-written half was the
    // incomplete one -- see set_mode_periodic() for what it got wrong.
    //
    // The counter has to be running before then, because set_mode_periodic()
    // reads it to place the first interrupt.
    if (mmio->general_caps.support_legacy()) {
        apic_mask_irq(apic_io_isa_to_global(ISA_IRQ_PIT));

        mmio->general_config.disable_cnf();
        mmio->main_counter_value = 0;
        mmio->general_config.enable_legacy_route(true);
        mmio->general_config.enable_cnf();

        HPET_LEGACY_IRQ_ENABLED = true;
        apic_unmask_irq(apic_io_isa_to_global(ISA_IRQ_PIT));
    }

    // Only now, with the counter enabled, is there anything to check.
    if (!hpet_counter_is_running()) {
        log::info("HPET: the main counter is not advancing.");
        return Status::BadState;
    }

    return Status::Ok;
}

INIT_CODE static auto setup_hpet_mmio(PhysAddr mmio) -> Status {
    using namespace mm;

    VirtAddr hpet_mmio = 0;
    auto result = VmAspace::kernel_aspace()->root_vma()->map_at(
        mmio, &hpet_mmio, PAGE_SIZE, MmuFlags::Readable | MmuFlags::Writable | MmuFlags::Discache,
        VmMapOption::Commit | VmMapOption::Pinned, "HPET");

    if (!result) {
        return result.unwrap_err();
    }

    HPET_MMIO = reinterpret_cast<decltype(HPET_MMIO)>(hpet_mmio);
    return Status::Ok;
}

/// From https://wiki.osdev.org/HPET#Initialization, we should do the following:
/// 1. Find HPET base address in 'HPET' ACPI table.
/// 2. Calculate HPET frequency (f = 10^15 / period).
/// 3. Save minimal tick (either from ACPI table or configuration register).
/// 4. Initialize comparators.
/// 5. Set ENABLE_CNF bit.
INIT_CODE auto hpet_init(PhysAddr mmio) -> Status {
    DEBUG_ASSERT(mmio);

    auto status = setup_hpet_mmio(mmio);
    if (Status::Ok != status) {
        log::info("Failed to create HPET MMIO: {}.", static_cast<u32>(status));
        return status;
    }

    status = hpet_init_internal();
    if (Status::Ok != status) {
        log::info("Failed to create HPET MMIO: {}.", static_cast<u32>(status));
        return status;
    }

    ClockSource::enroll(&HPET_CLOCK_SOURCE.high_level_source(), HPET_FREQUENCY, HPET_CLKSRC_RATING);

    auto &device = HPET_CHANNELS[0].device();
    ClockEventDevice::enroll(device, HPET_FREQUENCY, HPET_MIN_CYCLES, 0x7FFFFFFF);

    time::CLOCK_EVENT = &device;
    return Status::Ok;
}

} // namespace ours
