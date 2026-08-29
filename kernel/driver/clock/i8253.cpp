#include <arch/x86/io.hpp>
#include <ours/driver/i8253.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/time/clock_event_device.hpp>
#include <ustl/lazy_init.hpp>

namespace ours {
using time::ClockEventDevice;

constexpr static auto const PIT_MODE = 0x43;

constexpr static auto const PIT_CH0 = 0x40;

constexpr static auto const PIT_CH2 = 0x42;

constexpr static auto const PIT_LATCH = 0;

constexpr static auto const PIT_FREQUENCY = 1193182ul;

class I8253ClockDevice {
    using Self = I8253ClockDevice;

  public:
    constexpr I8253ClockDevice()
        : m_lock()
        , m_clkevt("I8253", &VTABLE,
                   ClockEventDevice::Feature::Periodic | ClockEventDevice::Feature::Oneshot, 0) {}

    constexpr ~I8253ClockDevice() = default;

    auto shutdown() -> void {
        Guard guard{m_lock};
        arch::outb(PIT_MODE, 0x30);
        arch::outb(PIT_CH0, 0);
        arch::outb(PIT_CH2, 0);
        arch::outb(PIT_MODE, 0x30);
    }

    auto set_mode_oneshot() -> Status {
        Guard guard{m_lock};
        arch::outb(PIT_MODE, 0x38);
        return Status::Ok;
    }

    auto set_mode_periodic() -> Status {
        Guard guard{m_lock};
        arch::outb(PIT_MODE, 0x34);
        arch::outb(PIT_CH0, static_cast<u8>(PIT_LATCH & 0xffu));        // LSB
        arch::outb(PIT_CH0, static_cast<u8>((PIT_LATCH >> 8) & 0xffu)); // MSB
        return Status::Ok;
    }

    auto set_next_event_tick(Ticks delta) -> Status {
        Guard guard{m_lock};
        arch::outb(PIT_CH0, static_cast<u8>(delta & 0xffu));        // LSB
        arch::outb(PIT_CH0, static_cast<u8>((delta >> 8) & 0xffu)); // MSB
        return Status::Ok;
    }

    auto device() -> ClockEventDevice * {
        return &m_clkevt;
    }

  private:
    sync::SpinLock m_lock;

    // It must be after |m_vtable|.
    ClockEventDevice m_clkevt;

    using Packer = ClockEventDevice::Vtable::Packer<&Self::m_clkevt>;

    constexpr static const ClockEventDevice::Vtable VTABLE{
        .set_next_event_tick = Packer::pack<&Self::set_next_event_tick>(),
        .set_next_event_time = nullptr,
        .set_mode_periodic = Packer::pack<&Self::set_mode_periodic>(),
        .set_mode_oneshot = Packer::pack<&Self::set_mode_oneshot>(),
        .stop_oneshot = nullptr,
        .resume = nullptr,
        .suspend = nullptr,
        .shutdown = Packer::pack<&Self::shutdown>(),
        .broadcast = nullptr,
    };
};

static ustl::LazyInit<I8253ClockDevice> s_i8253_clkevt;
ClockEventDevice *g_i8253_clkevt;

INIT_CODE auto i8253_init(bool oneshot) -> void {
    s_i8253_clkevt.init();
    g_i8253_clkevt = s_i8253_clkevt->device();

    ClockEventDevice::entroll(*g_i8253_clkevt, PIT_FREQUENCY, 0xF, 0x7FFF);
}

} // namespace ours
