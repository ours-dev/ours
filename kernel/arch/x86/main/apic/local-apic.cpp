#include <ours/arch/apic.hpp>
#include <ours/arch/x86/feature.hpp>

#include <ours/irq/irq_chip.hpp>
#include <ours/time/timer-queue.hpp>
#include <ours/time/clock-source.hpp>
#include <ours/time/clock-event-device.hpp>

#include <ours/mem/vm_area.hpp>
#include <ours/mem/vm_mapping.hpp>

#include <ours/mem/cpu-local.hpp>

#include <arch/x86/apic/xapic.hpp>
#include <arch/x86/msr.hpp>
#include <arch/x86/interrupt.hpp>
#include <arch/tick.hpp>

#include <logz4/log.hpp>
#include <ustl/algorithms/generation.hpp>
#include <gktl/init_hook.hpp>

namespace ours {
namespace {
    void *s_apic_mmio_base;
    bool s_x2apic_enabled = false;

    CPU_LOCAL
    u32 s_apic_id;

    auto current_apic_id() -> u32 {
        return CpuLocal::read(s_apic_id);
    }

    ustl::Array<isize, MAX_CPU> s_cpu_to_apicid;

    /// FIXME(SmallHuaZi): 
    ustl::Array<CpuNum, MAX_CPU> s_apicid_to_cpu;   
} // namespace anonymous

    using namespace irq;
    using namespace time;
    using mem::CpuLocal;

    INIT_CODE
    auto assign_cpunum_for_apic(u32 apicid) -> CpuNum {
        INIT_DATA
        static CpuMask assigned;

        auto const cpunum = assigned.count();
        if (cpunum >= MAX_CPU) {
            return kInvalidCpuNum;
        }

        assigned.set(cpunum, true);
        Cpu::set_state(Cpu::State::Possible);
        s_apicid_to_cpu[apicid] = cpunum;
        s_cpu_to_apicid[cpunum] = apicid;
        return cpunum;
    }

    auto cpunum_to_apic_id(CpuNum cpunum) -> u32 {
        DEBUG_ASSERT(s_cpu_to_apicid[cpunum] != kInvalidCpuNum);
        return s_cpu_to_apicid[cpunum];
    }

    auto apic_id_to_cpunum(u32 apicid) -> CpuNum {
        DEBUG_ASSERT(s_apicid_to_cpu[apicid] != arch::kInvalidApicId);
        return s_apicid_to_cpu[apicid];
    }

    static arch::XApic s_xapic;
    arch::XApic *g_xapic;

    auto apic_issue_eoi() -> void {
        s_xapic.issue_eoi();
    }

    struct IrqChip s_xapic_chip {
        .mask = [](IrqData &data) -> void {
            s_xapic.mask(arch::XApicRegType::LvtLogicalInt0);
        },
        .unmask = [](IrqData &data) -> void {
            s_xapic.unmask(arch::XApicRegType::LvtLogicalInt0);
        },
        .issue_ack = [](IrqData &data) -> void {
            s_xapic.issue_eoi();
        },
        .name = "XApic",
    };

    /// This is the local APIC timer event device for non-deadline TSC devices.
    class ApicTimerEvent {
        using Self = ApicTimerEvent;
      public:
        ApicTimerEvent()
            : canary_()
            , m_clkevt("LVT", &Self::s_vtable, ClockEventDevice::Feature::Oneshot, 0)
        {}

        ~ApicTimerEvent() = default;

        auto set_mode_oneshot() -> Status {
            canary_.verify();
            return setup_timer(s_apic_period, true, false);
        }

        auto set_mode_periodic() -> Status {
            canary_.verify();
            return setup_timer(s_apic_period, false, false);
        }

        auto broadcast(CpuMask const &mask) -> void {
            canary_.verify();
            apic_send_ipi_mask(mask, arch::IrqVec::ApicTimer, arch::ApicDeliveryMode::Fixed);
        }

        auto shutdown() -> void {
            canary_.verify();
            s_xapic.disable_tsc(arch::IrqVec::ApicTimer);
            s_xapic.write_reg(arch::XApicRegType::TimerInitCount, 0);
        }

        auto set_next_event_tick(Ticks delta) -> Status;

        auto set_next_event_tick_deadline(Ticks delta) -> Status;

        auto shutdown_deadline() -> void;

        static auto setup_timer(u32 clocks, bool oneshot, bool masked) -> Status;

        gktl::Canary m_canary;
        ClockEventDevice m_clkevt;

        /// How many cycles in 1/HZ second.
        inline static usize s_apic_period;
        inline static usize s_divisor;

        using Packer = ClockEventDevice::Vtable::Packer<&ApicTimerEvent::m_clkevt>;
        inline static ClockEventDevice::Vtable s_vtable {
            .set_next_event_tick = Packer::pack<&ApicTimerEvent::set_next_event_tick>(),
            .set_mode_periodic = Packer::pack<&ApicTimerEvent::set_mode_periodic>(),
            .set_mode_oneshot = Packer::pack<&ApicTimerEvent::set_mode_oneshot>(),
            .shutdown = Packer::pack<&ApicTimerEvent::shutdown>(),
            .broadcast = Packer::pack<&ApicTimerEvent::broadcast>(),
        };
    };

    auto ApicTimerEvent::set_next_event_tick(Ticks delta) -> Status {
        canary_.verify();

        u8 const highest_set_bit = delta < 2 ? 0 : ustl::bit_width<usize>(delta) - 1;
        u8 divisor = s_divisor;
        usize count = delta;
        if (highest_set_bit > 31) {
            // The current resolution is not statisfied, we need to adjust the divisor.
            u8 extra_bits = highest_set_bit - 31;
            divisor <<= extra_bits;
            if (divisor > 128) {
                divisor = 128;
                count = ustl::NumericLimits<u32>::max();
            } else {
                count >>= extra_bits;
                if (!count) {
                    count = 1;
                }
            }

            CXX11_CONSTEXPR
            static u32 const kDivisor[] = {0xb, 0, 1, 2, 3, 8, 9, 10};

            if (divisor == 0 || divisor > std::size(kDivisor)) {
                return Status::InvalidArguments;
            }

            divisor = kDivisor[divisor - 1];
        }

        s_xapic.write_reg(arch::XApicRegType::TimerInitCount, count);
        s_xapic.write_reg(arch::XApicRegType::TimerDivConf, divisor);
        return Status::Ok;
    }

    auto ApicTimerEvent::set_next_event_tick_deadline(Ticks delta) -> Status {
        canary_.verify();

        if (delta < 0) {
            return Status::InvalidArguments;
        }

        auto const now = arch::Tick::get();
        // Deadline timer is set by TSC, so we can just set it directly.
        s_xapic.set_tsc_deadline(now.tsc + delta);
        return Status::Ok;
    }

    auto ApicTimerEvent::shutdown_deadline() -> void {
        canary_.verify();
        // Disable the TSC deadline timer.
        s_xapic.disable_tsc(arch::IrqVec::ApicTimer);
        s_xapic.write_reg(arch::XApicRegType::TimerInitCount, 0);
    }

    auto ApicTimerEvent::setup_timer(u32 clocks, bool oneshot, bool masked) -> Status {
        using namespace arch;
        auto const support_tsc_deadline = x86_has_feature(CpuFeatureType::TscDeadlineTimer);

        // When TSC deadline timer is supported by system, we need to update our Vtable.
        if (support_tsc_deadline) {
            s_vtable.set_next_event_tick = Packer::pack<&Self::set_next_event_tick_deadline>();
            s_vtable.shutdown = Packer::pack<&Self::shutdown_deadline>();
        }

        if (!oneshot) {
            s_xapic.enable_tsc(IrqVec::ApicTimer, XApic::TscMode::Periodic);
        } else if (support_tsc_deadline) {
            s_xapic.enable_tsc(IrqVec::ApicTimer, XApic::TscMode::Deadline);
        }

        if (masked) {
            s_xapic.mask(arch::XApicRegType::LvtTimer);
        } else {
            s_xapic.unmask(arch::XApicRegType::LvtTimer);
        }

        s_xapic.write_reg(arch::XApicRegType::TimerInitCount, clocks);
        return Status::Ok;
    }

    CPU_LOCAL
    static ApicTimerEvent s_clkdev_storage;

    static ClockEventDevice *s_lapic_timer_event;

    auto apic_handle_timer_irq() -> void {
        DEBUG_ASSERT(s_lapic_timer_event);

        apic_issue_eoi();
        s_lapic_timer_event->handle_event();
    }

    FORCE_INLINE
    static auto apic_timer_current_count() -> u64 {
        return s_xapic.read_reg(arch::XApicRegType::TimerCurrentCount);
    }

    struct Calibration {
        auto start() -> void {
            apic_cycle_first = apic_timer_current_count();
            tsc_cycle_first = arch::Tick::get().tsc;
        }

        auto finish() -> void {
            apic_cycle_last = apic_timer_current_count();
            tsc_cycle_last = arch::Tick::get().tsc;
        }

        usize apic_cycle_first = 0;
        usize apic_cycle_last = 0;

        usize tsc_cycle_first = 0;
        usize tsc_cycle_last = 0;
    };
    INIT_DATA
    static Calibration s_calibration;

    INIT_CODE
    static auto calibrate_apic_clock_single(u32 periods, u32 apic_advisor) -> u32 {
        CXX11_CONSTEXPR
        static auto const kApicInitCount = ustl::NumericLimits<u32>::max();

        Calibration calibration{};
        auto status = ApicTimerEvent::setup_timer(kApicInitCount, true, true);
        ASSERT(Status::Ok == status);

        calibration.start();
        auto const tick_start = ClockSource::Default::read();
        while (true) {
            auto const elapse = ClockSource::Default::read() - tick_start;
            if (elapse >= periods) {
                break;
            }
        }
        calibration.finish();

        return calibration.apic_cycle_last - calibration.apic_cycle_first;
    }

    INIT_CODE
    auto apic_calibrate_clock() -> bool {
        CXX11_CONSTEXPR
        static auto const kNumTrails = 4;

        CXX11_CONSTEXPR
        static auto const kInvalidTickNum = ustl::NumericLimits<u32>::max();

        u32 best_time[kNumTrails];
        ustl::algorithms::fill_n(best_time, kNumTrails, kInvalidTickNum);

        u32 periods[kNumTrails] = {1, 2, 4, 8};
        u32 apic_advisor = 1;

        DEBUG_ASSERT(!arch::irqs_disabled());
        for (auto trail = 0; trail < kNumTrails; ++trail) {
            for (auto i = 0; i < 2; ++i) {
                u32 elapsed_ticks = calibrate_apic_clock_single(periods[trail], apic_advisor);
                log::trace("Calibration result for duration {} ms: {} ticks/ms.", 
                    periods[trail], elapsed_ticks / periods[trail]);

                if (elapsed_ticks < best_time[trail]) {
                    best_time[trail] = elapsed_ticks;
                } else if (best_time[trail] == kInvalidTickNum) {
                    i -= 1;
                    apic_advisor = u8(apic_advisor << 1);
                }
            }
        }

        auto const apic_period = (best_time[1] - best_time[0]) / (periods[1] - periods[0]);
        ApicTimerEvent::s_apic_period = apic_period * apic_advisor;
        ApicTimerEvent::s_divisor = apic_advisor;
        return true;
    }

    INIT_CODE
    auto current_apic_id_early() -> u32 {
        return s_xapic.id();
    }

    INIT_CODE
    static auto init_apic_timer() -> void {
        using namespace arch;

        // By default to let timer work on periodic mode.
        s_xapic.enable_tsc(IrqVec::ApicTimer, XApic::TscMode::Periodic);
        s_xapic.mask(XApicRegType::LvtTimer);

        ClockEventDevice *clkdev;
        s_lapic_timer_event = CpuLocal::access(&s_clkdev_storage.m_clkevt);
        ClockEventDevice::entroll(*clkdev);
    }

    /// Initialize APIC performance monitoring interrupt.
    INIT_CODE
    static auto init_apic_pmi() -> void {
        using namespace arch;
        s_xapic.write_reg(XApicRegType::LvtPmi, IrqVec::ApicPmi);
        s_xapic.mask(arch::XApicRegType::LvtPmi);
    }

    INIT_CODE
    static auto init_apic_error() -> void {
        using namespace arch;
        s_xapic.write_reg(XApicRegType::LvtError, IrqVec::ApicError);
        s_xapic.write_reg(XApicRegType::Esr, 0);
    }

    INIT_CODE
    auto init_local_apic_percpu() -> void {
        using namespace arch;
        auto apic_base = MsrIo::read<usize>(MsrRegAddr::IA32ApicBase);
        if (apic_base & arch::kIa32ApicBaseBsp) {
            if (x86_has_feature(CpuFeatureType::X2Apic)) {
                log::trace("X2-APIC enabled");
            }
        }

        apic_base |= arch::kIa32ApicBaseXapicEnable;
        if (s_x2apic_enabled) {
            apic_base |= arch::kIa32ApicBaseX2ApicEnable;
        }

        // Enable X[2]-APIC
        MsrIo::write<usize>(MsrRegAddr::IA32ApicBase, apic_base);

        // Save ID of this local APIC
        CpuLocal::write(s_apic_id, s_xapic.id());

        if (!(apic_base & arch::kIa32ApicBaseBsp)) {
            init_apic_timer();
        }
        init_apic_error();
        init_apic_pmi();
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
        using namespace mem;
        auto result = VmAspace::kernel_aspace()->root_vma()->map_at(
            arch::kApicPhysBase, 0, PAGE_SIZE, 
            // `Discache` is required to prevent 
            MmuFlags::Discache | MmuFlags::Readable | MmuFlags::Writable, 
            VmMapOption::Commit | VmMapOption::Pinned, "Local-APIC"
        );
        DEBUG_ASSERT(result, "Failed to allocate MMIO for BP's Local APIC");

        auto mmio_base(reinterpret_cast<u32 *>((*result)->base()));

        s_xapic.init(mmio_base);

        init_local_apic_percpu();
        g_xapic = &s_xapic;
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

} // namespace ours::irq