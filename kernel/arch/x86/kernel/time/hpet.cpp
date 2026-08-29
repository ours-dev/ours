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

class HpetChannel;

static arch::HpetRegs *s_hpet_mmio;
static usize s_hpet_frequency;
static ustl::views::Span<HpetChannel> s_hpet_channels;
[[maybe_unused]]
static bool s_hpet_legacy_irq_enabled;

class HpetClockSource {
    using Self = HpetClockSource;

  public:
    HpetClockSource() : m_canary(), m_spinlock(), m_clksrc("HPET-CLKSRC", &VTABLE), m_cache(0) {}

    auto read() -> u64 {
        m_canary.verify<Self>();

        u64 const oldvalue = m_cache;
        if (::arch::AutoInterruptDisabler guard; m_spinlock.try_acquire()) {
            auto const value = s_hpet_mmio->main_counter_value;
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
        s_hpet_mmio->general_config.disable_cnf();
        return Status::Ok;
    }

    auto resume() -> Status {
        m_canary.verify<Self>();
        s_hpet_mmio->general_config.enable_cnf();
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
    constexpr static const ClockSource::Vtable VTABLE{
        .read = Packer::pack<&Self::read>(),
        .enable = nullptr,
        .disable = nullptr,
        .suspend = Packer::pack<&Self::suspend>(),
        .resume = Packer::pack<&Self::resume>(),
    };
};
static HpetClockSource s_hpet_clock_source;

class HpetChannel {
    using Self = HpetChannel;

  public:
    HpetChannel()
        : m_canary()
        , id_(0)
        , regs_mmio_(0)
        , m_clkevt("HPET", &Self::VTABLE,
                   ClockEventDevice::Feature::Periodic | ClockEventDevice::Feature::Oneshot, 0) {}

    ~HpetChannel() = default;

    auto init(usize id, arch::HpetTimerRegs *mmio) -> void {
        m_canary.verify<Self>();
        id_ = id;
        regs_mmio_ = mmio;
        m_clkevt.set_irqnum(static_cast<VIrqNum>(mmio->conf_caps.irq()));
    }

    auto set_next_event_time(TimePoint<> expires) -> Status {
        m_canary.verify<Self>();
        return Status::Ok;
    }

    auto set_next_event_tick(Ticks delta) -> Status {
        m_canary.verify<Self>();
        auto const now = s_hpet_mmio->main_counter_value;
        regs_mmio_->comparator_value = now + delta;
        return Status::Ok;
    }

    auto set_mode_oneshot() -> Status {
        m_canary.verify<Self>();
        regs_mmio_->conf_caps.set_mode_oneshot();
        return Status::Ok;
    }

    auto set_mode_periodic() -> Status {
        m_canary.verify<Self>();
        if (!regs_mmio_->conf_caps.support_periodic()) {
            return Status::Unsupported;
        }
        regs_mmio_->conf_caps.set_mode_periodic();
        return Status::Ok;
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
    ClockEventDevice m_clkevt;

    using Packer = ClockEventDevice::Vtable::Packer<&Self::m_clkevt>;
    constexpr static const ClockEventDevice::Vtable VTABLE{
        .set_next_event_tick = Packer::pack<&Self::set_next_event_tick>(),
        .set_next_event_time = Packer::pack<&Self::set_next_event_time>(),
        .set_mode_periodic = Packer::pack<&Self::set_mode_periodic>(),
        .set_mode_oneshot = Packer::pack<&Self::set_mode_oneshot>(),
        .stop_oneshot = nullptr,
        .resume = Packer::pack<&Self::resume>(),
        .suspend = nullptr,
        .shutdown = nullptr,
        .broadcast = nullptr,
    };
};

INIT_CODE static auto hpet_init_internal() -> Status {
    DEBUG_ASSERT(s_hpet_mmio);
    auto const mmio = s_hpet_mmio;

    if (!mmio->general_caps.support_64bit()) {
        log::info("HPET: Unsupport 64-bit.");
        return Status::Unsupported;
    }

    auto const num_channels = mmio->general_caps.num_timers();
    if (num_channels < 2) {
        log::info("HPET: Not enough timers to support system requirements.");
        return Status::NoResource;
    }

    auto const channels = new (mm::GAF_KERNEL) HpetChannel[num_channels];
    if (!channels) {
        return Status::OutOfMem;
    }

    s_hpet_channels = ustl::views::make_span(channels, num_channels);
    for (auto i = 0; i < num_channels; ++i) {
        s_hpet_channels[i].init(i, &mmio->timers[i]);
    }

    constexpr auto const FEMTOSECONDS_PER_SECOND = duration_cast<Femtoseconds>(Seconds(1)).count();
    auto const period = mmio->general_caps.period();
    if (period == 0) {
        log::info("HPET: Invalid period.");
        return Status::InternalError;
    }

    // HZ: How many ticks in one second.
    s_hpet_frequency = (FEMTOSECONDS_PER_SECOND + period - 1) / period;
    log::info("HPET Frequency = {}", s_hpet_frequency);

    if (mmio->general_caps.support_legacy()) {
        apic_mask_irq(apic_io_isa_to_global(0));

        mmio->general_config.disable_cnf();
        mmio->main_counter_value = 0;
        mmio->general_config.enable_legacy_route(true);
        mmio->timers[0].conf_caps.set_mode_periodic().mask(false);
        mmio->timers[0].comparator_value = s_hpet_frequency / HZ; // MS
        mmio->general_config.enable_cnf();

        s_hpet_legacy_irq_enabled = true;
        apic_unmask_irq(apic_io_isa_to_global(0));
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

    s_hpet_mmio = reinterpret_cast<decltype(s_hpet_mmio)>(hpet_mmio);
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

    ClockSource::enroll(&s_hpet_clock_source.high_level_source(), s_hpet_frequency);

    auto &device = s_hpet_channels[0].device();
    ClockEventDevice::entroll(device, s_hpet_frequency, HPET_MIN_CYCLES, 0x7FFFFFFF);

    time::g_clock_event = &device;
    return Status::Ok;
}

} // namespace ours
