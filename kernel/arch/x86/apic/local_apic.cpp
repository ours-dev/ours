#include <arch/interrupt.hpp>
#include <arch/tick.hpp>
#include <arch/x86/apic/xapic.hpp>
#include <arch/x86/msr.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ours/arch/apic.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/cpu.hpp>
#include <ours/irq/irq_chip.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/time/clock_event_device.hpp>
#include <ours/time/clock_source.hpp>
#include <array>
#include <ustl/bit.hpp>
#include <ustl/limits.hpp>
#include <algorithm>

namespace ours {
namespace {
    bool s_x2apic_enabled = false;

    CPU_LOCAL
    u32 s_apic_id;

    constinit auto s_cpu_to_apicid = []() constexpr -> std::array<u32, MAX_CPU> {
        std::array<u32, MAX_CPU> apicids{};
        for (auto &apicid: apicids) {
            apicid = arch::kInvalidApicId;
        }
        return apicids;
    }();

    constinit auto s_apicid_to_cpu = []() constexpr -> std::array<CpuNum, MAX_CPU> {
        std::array<CpuNum, MAX_CPU> cpunums{};
        for (auto &cpunum: cpunums) {
            cpunum = INVALID_CPU_NUM;
        }
        return cpunums;
    }();
} // namespace

auto current_apic_id() -> u32 {
    return mm::CpuLocal::read(s_apic_id);
}

using namespace irq;
using namespace time;
using mm::CpuLocal;

INIT_CODE
auto assign_cpunum_for_apic(u32 apicid) -> CpuNum {
    INIT_DATA
    static CpuMask assigned;

    if (apicid >= MAX_CPU) {
        return INVALID_CPU_NUM;
    }

    if (s_apicid_to_cpu[apicid] != INVALID_CPU_NUM) {
        return s_apicid_to_cpu[apicid];
    }

    auto const assigned_count = assigned.count();
    if (assigned_count >= MAX_CPU) {
        return INVALID_CPU_NUM;
    }

    auto const cpunum = static_cast<CpuNum>(assigned_count);
    assigned.set(cpunum, true);
    Cpu::set_state(cpunum, Cpu::State::Possible);
    s_apicid_to_cpu[apicid] = cpunum;
    s_cpu_to_apicid[cpunum] = apicid;
    return cpunum;
}

auto cpunum_to_apic_id(CpuNum cpunum) -> u32 {
    if (cpunum >= MAX_CPU) {
        return arch::kInvalidApicId;
    }

    auto const apicid = s_cpu_to_apicid[cpunum];
    DEBUG_ASSERT(apicid != arch::kInvalidApicId);
    return apicid;
}

auto apic_id_to_cpunum(u32 apicid) -> CpuNum {
    if (apicid >= MAX_CPU) {
        return INVALID_CPU_NUM;
    }

    auto const cpunum = s_apicid_to_cpu[apicid];
    DEBUG_ASSERT(cpunum != INVALID_CPU_NUM);
    return cpunum;
}

static arch::XApic s_xapic;
arch::XApic *g_xapic;

auto apic_issue_eoi() -> void {
    s_xapic.issue_eoi();
}

[[maybe_unused]]
IrqChip s_xapic_chip{
    .mask = [](IrqData &data) -> void { s_xapic.mask(arch::XApicRegType::LvtLogicalInt0); },
    .unmask = [](IrqData &data) -> void { s_xapic.unmask(arch::XApicRegType::LvtLogicalInt0); },
    .issue_ack = [](IrqData &data) -> void { s_xapic.issue_eoi(); },
    .name = "XApic",
};

/// This is the local APIC timer event device for non-deadline TSC devices.
class ApicTimerEvent {
    using Self = ApicTimerEvent;

  public:
    ApicTimerEvent()
        : m_canary(this), m_clkevt("LVT", &Self::s_vtable, ClockEventDevice::Feature::Oneshot, 0) {}

    ~ApicTimerEvent() = default;

    auto set_mode_oneshot() -> Status {
        m_canary.verify<Self>();
        return setup_timer(s_apic_period, true, false);
    }

    auto set_mode_periodic() -> Status {
        m_canary.verify<Self>();
        return setup_timer(s_apic_period, false, false);
    }

    auto broadcast(CpuMask const &mask) -> void {
        m_canary.verify<Self>();
        apic_send_ipi_mulitcast(mask, arch::IrqVec::ApicTimer, arch::ApicDeliveryMode::Fixed);
    }

    auto shutdown() -> void {
        m_canary.verify<Self>();
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
    inline static u32 s_apic_period;
    inline static u8 s_divisor;

    using Packer = ClockEventDevice::Vtable::Packer<&ApicTimerEvent::m_clkevt>;
    inline static ClockEventDevice::Vtable s_vtable{
        .set_next_event_tick = Packer::pack<&ApicTimerEvent::set_next_event_tick>(),
        .set_next_event_time = nullptr,
        .set_mode_periodic = Packer::pack<&ApicTimerEvent::set_mode_periodic>(),
        .set_mode_oneshot = Packer::pack<&ApicTimerEvent::set_mode_oneshot>(),
        .stop_oneshot = nullptr,
        .resume = nullptr,
        .suspend = nullptr,
        .shutdown = Packer::pack<&ApicTimerEvent::shutdown>(),
        .broadcast = Packer::pack<&ApicTimerEvent::broadcast>(),
    };
};

auto ApicTimerEvent::set_next_event_tick(Ticks delta) -> Status {
    m_canary.verify<Self>();

    auto const highest_set_bit = delta < 2 ? 0 : ustl::bit_width(delta) - 1;
    auto divisor = static_cast<usize>(s_divisor);
    usize count = delta;
    if (highest_set_bit > 31) {
        // The current resolution is not statisfied, we need to adjust the divisor.
        auto const extra_bits = highest_set_bit - 31;
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

        constexpr static u32 const kDivisor[] = {0xb, 0, 1, 2, 3, 8, 9, 10};

        if (divisor == 0 || divisor > (sizeof(kDivisor) / sizeof(kDivisor[0]))) {
            return Status::InvalidArguments;
        }

        divisor = kDivisor[divisor - 1];
    }

    s_xapic.write_reg(arch::XApicRegType::TimerInitCount, count);
    s_xapic.write_reg(arch::XApicRegType::TimerDivConf, divisor);
    return Status::Ok;
}

auto ApicTimerEvent::set_next_event_tick_deadline(Ticks delta) -> Status {
    m_canary.verify<Self>();

    if (delta < 0) {
        return Status::InvalidArguments;
    }

    auto const now = arch::Tick::get();
    // Deadline timer is set by TSC, so we can just set it directly.
    s_xapic.set_tsc_deadline(now.tsc + delta);
    return Status::Ok;
}

auto ApicTimerEvent::shutdown_deadline() -> void {
    m_canary.verify<Self>();
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
INIT_CODE
static auto calibrate_apic_clock_single(u32 periods, u32 apic_advisor) -> u32 {
    constexpr static auto const kApicInitCount = ustl::NumericLimits<u32>::max();

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

    return static_cast<u32>(calibration.apic_cycle_last - calibration.apic_cycle_first);
}

INIT_CODE
auto apic_calibrate_clock() -> bool {
    constexpr static auto const kNumTrails = 4;

    constexpr static auto const kInvalidTickNum = ustl::NumericLimits<u32>::max();

    u32 best_time[kNumTrails];
    std::fill_n(best_time, kNumTrails, kInvalidTickNum);

    u32 periods[kNumTrails] = {1, 2, 4, 8};
    u32 apic_advisor = 1;

    DEBUG_ASSERT(!arch::irqs_disabled());
    for (auto trail = 0; trail < kNumTrails; ++trail) {
        for (auto i = 0; i < 2; ++i) {
            u32 elapsed_ticks = calibrate_apic_clock_single(periods[trail], apic_advisor);
            log::trace("Calibration result for duration {} ms: {} ticks/ms.", periods[trail],
                       elapsed_ticks / periods[trail]);

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
    ApicTimerEvent::s_divisor = static_cast<u8>(apic_advisor);
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

    s_lapic_timer_event = &CpuLocal::access(s_clkdev_storage).m_clkevt;
    ClockEventDevice::entroll(*s_lapic_timer_event);
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
    auto result = mm::VmAspace::kernel_aspace()->root_vma()->map_at(
        arch::kApicPhysBase, 0, PAGE_SIZE,
        // `Discache` is required to prevent
        mm::MmuFlags::Discache | mm::MmuFlags::Readable | mm::MmuFlags::Writable,
        mm::VmMapOption::Commit | mm::VmMapOption::Pinned, "Local-APIC");
    DEBUG_ASSERT(result, "Failed to allocate MMIO for BP's Local APIC");

    auto mmio_base(reinterpret_cast<volatile u32 *>((*result)->base()));

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

} // namespace ours
