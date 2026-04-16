#include <ours/arch/hpet.hpp>
#include <ours/arch/apic.hpp>
#include <ours/arch/time.hpp>


#include <ours/assert.hpp>
#include <ours/mem/cpu-local.hpp>
#include <ours/sync/spin-lock.hpp>
#include <ours/lockdep.hpp>

#include <ours/time/clock-event-device.hpp>
#include <ours/time/clock-source.hpp>
#include <ours/time/cfg.hpp>

// This is a very inconvenient way 
#include <ours/mem/vm_aspace.hpp>
#include <ours/mem/vm_area.hpp>
#include <ours/mem/vm_mapping.hpp>

#include <acpi/structures/hpet.hpp>
#include <logz4/log.hpp>
#include <ktl/new.hpp>
#include <gktl/canary.hpp>
#include <ustl/chrono/duration.hpp>
#include <ustl/traits/cv.hpp>
#include <ustl/views/span.hpp>

using namespace ustl::chrono;

namespace ours {
    using time::ClockSource;
    using time::ClockEventDevice;

    CXX11_CONSTEXPR
    static auto kHpetMinCycles = 128;

    class HpetChannel;

    static acpi::HpetRegs *s_hpet_mmio;
    static usize s_hpet_frequency;
    static u64 s_hpet_boot_config;
    static ustl::views::Span<HpetChannel> s_hpet_channels;
    static bool s_hpet_legacy_irq_enabled;

    enum class HpetMode {
        Unused,
        Legacy,
        Oneshot,
    };

    class HpetClockSource {
        using Self = HpetClockSource;
      public:
        HpetClockSource()
            : m_canary()
            , m_spinlock()
            , m_mode(HpetMode::Legacy)
            , m_clksrc("HPET-CLKSRC", &kVtable) 
            , m_cache(0)
        {}

        auto read() -> u64 {
            m_canary.verify<Self>();

            u64 const oldvalue = m_cache;
            if (arch::AutoInterruptDisabler guard; m_spinlock.try_acquire()) {
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
        HpetMode m_mode;
        ClockSource m_clksrc;
        usize m_cache;

        using Packer = ClockSource::Vtable::Packer<&Self::m_clksrc>;
        CXX11_CONSTEXPR
        static const ClockSource::Vtable kVtable {
            .read = Packer::pack<&Self::read>(),
            .suspend = Packer::pack<&Self::suspend>(),
            .resume = Packer::pack<&Self::resume>(),
        };
    };
    static HpetClockSource s_hpet_clock_source;

    class HpetChannel {
        using Self = HpetChannel;
      public:
        HpetChannel()
            : canary_() 
            , inuse_(false)
            , id_(-1)
            , cpu_(kInvalidCpuNum)
            , regs_mmio_(0)
            , m_clkevt("HPET", &Self::kVtable,
                      ClockEventDevice::Feature::Periodic | ClockEventDevice::Feature::Oneshot, 0)
        {}

        ~HpetChannel() = default;

        auto init(usize id, acpi::HpetTimerRegs *mmio) -> void {
            canary_.verify();
            id_ = id;
            regs_mmio_ = mmio;
            m_clkevt.set_irqnum(mmio->conf_caps.irq());
        }

        auto set_next_event_time(TimePoint<> expires) -> Status {
            canary_.verify();
            return Status::Ok;
        }

        auto set_next_event_tick(Ticks delta) -> Status {
            canary_.verify();
            auto const now = s_hpet_mmio->main_counter_value;
            regs_mmio_->comparator_value = now + delta;
            return Status::Ok;
        }

        auto set_mode_oneshot() -> Status {
            canary_.verify();
            regs_mmio_->conf_caps.set_mode_oneshot();
            return Status::Ok;
        }

        auto set_mode_periodic() -> Status {
            canary_.verify();
            if (!regs_mmio_->conf_caps.support_periodic()) {
                return Status::Unsupported;
            }
            regs_mmio_->conf_caps.set_mode_periodic();
            return Status::Ok;
        }

        auto resume() -> Status {
            canary_.verify();
            regs_mmio_->conf_caps.mask(true);
            return Status::Ok;
        }

        FORCE_INLINE
        auto device() -> ClockEventDevice & {
            return m_clkevt;
        }
      private:
        gktl::Canary m_canary;
        u32 inuse_;
        u32 id_;
        CpuNum cpu_;
        acpi::HpetTimerRegs *regs_mmio_;
        ClockEventDevice m_clkevt;

        using Packer = ClockEventDevice::Vtable::Packer<&Self::m_clkevt>;
        CXX11_CONSTEXPR
        static const ClockEventDevice::Vtable kVtable {
            .set_next_event_tick = Packer::pack<&Self::set_next_event_tick>(),
            .set_next_event_time = Packer::pack<&Self::set_next_event_time>(),
            .set_mode_periodic = Packer::pack<&Self::set_mode_periodic>(),
            .set_mode_oneshot = Packer::pack<&Self::set_mode_oneshot>(),
            .resume = Packer::pack<&Self::resume>()
        };
    };

    INIT_CODE
    static auto hpet_init_internal() -> Status {
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

        auto const channels = new (mem::kGafKernel) HpetChannel[num_channels];
        if (!channels) {
            return Status::OutOfMem;
        }

        s_hpet_channels = ustl::views::make_span(channels, num_channels);
        for (auto i = 0; i < num_channels; ++i) {
            s_hpet_channels[i].init(i, &mmio->timers[i]);
        }

        CXX11_CONSTEXPR
        auto const kFemtosecondsPerSecond = duration_cast<Femtoseconds>(Seconds(1)).count();
        auto const period = mmio->general_caps.period(); 
        if (period == 0) {
            log::info("HPET: Invalid period.");
            return Status::InternalError;
        }

        // HZ: How many ticks in one second.
        s_hpet_frequency = (kFemtosecondsPerSecond + period - 1) / period;
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

    INIT_CODE
    static auto setup_hpet_mmio(PhysAddr mmio) -> Status {
        using namespace mem;

        int *x;
        void *y = x;

        VirtAddr hpet_mmio = 0;
        auto result = VmAspace::kernel_aspace()->root_vma()->map_at(
            mmio, 
            &hpet_mmio,
            PAGE_SIZE,
            MmuFlags::Readable | MmuFlags::Writable | MmuFlags::Discache,
            VmMapOption::Commit | VmMapOption::Pinned,
            "HPET"
        );

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
    INIT_CODE
    auto hpet_init(PhysAddr mmio) -> Status {
        DEBUG_ASSERT(mmio);

        auto status = setup_hpet_mmio(mmio);
        if (Status::Ok != status) {
            log::info("Failed to create HPET MMIO: {}.", to_string(status));
            return status;
        }

        status = hpet_init_internal();
        if (Status::Ok != status) {
            log::info("Failed to create HPET MMIO: {}.", to_string(status));
            return status;
        }

        ClockSource::enroll(&s_hpet_clock_source.high_level_source(), s_hpet_frequency);

        auto &device = s_hpet_channels[0].device();
        ClockEventDevice::entroll(device, s_hpet_frequency, kHpetMinCycles, 0x7FFFFFFF);

        time::g_clock_event = &device;
        return Status::Ok;
    }

} // namespace ours::time