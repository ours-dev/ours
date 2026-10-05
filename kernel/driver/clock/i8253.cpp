#include <ours/driver/i8253.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock_event_device.hpp>

#include <arch/x86/io.hpp>
#include <ustl/lazy_init.hpp>

namespace ours {
using time::ClockEventDevice;

constexpr static auto PIT_MODE = 0x43;

constexpr static auto PIT_CH0 = 0x40;

/// The 8254's input clock: 105/88 MHz, the classic 1.193182 MHz.
constexpr static auto PIT_FREQUENCY = 1193182ul;

/// The reload value for periodic mode: how many input cycles make one 1/HZ
/// period, rounded to nearest.
///
/// It used to be 0, which the 8254 reads as 65536 -- the largest divisor,
/// giving the DOS-era 18.2 Hz tick rather than HZ. Everything downstream
/// assumed one interrupt per NSEC_PER_TICK, so jiffies and every timeout
/// built on them ran roughly five times slow.
constexpr static auto PIT_LATCH = (PIT_FREQUENCY + time::HZ / 2) / time::HZ;

/// The PIT's place on the clock-event rating scale. Above the local APIC's
/// 100, which is harmless: a per-CPU device displaces a global one regardless
/// of rating, see TickDevice::check_preferred().
constexpr static auto PIT_CLKEVT_RATING = ClockEventDevice::Rating(110);

class I8253ClockDevice {
    using Self = I8253ClockDevice;

  public:
    constexpr I8253ClockDevice()
        : m_lock()
        , m_clkevt("I8253", &time::CLKEVT_VTABLE_OF<Self, &Self::m_clkevt>,
                   ClockEventDevice::Feature::Periodic | ClockEventDevice::Feature::Oneshot,
                   PIT_CLKEVT_RATING, nullptr) {}

    constexpr ~I8253ClockDevice() = default;

    /// The shutdown sequence: select channel 0, mode 0 (interrupt on
    /// terminal count), then write a zero reload value as two bytes.
    ///
    /// Both bytes go to channel 0. The second one used to go to PIT_CH2 --
    /// the PC speaker's channel -- which left channel 0 half-programmed,
    /// waiting for its high byte, and poked the speaker gate instead.
    auto shutdown() -> void {
        Guard<sync::SpinLock, IrqSave> guard{m_lock};
        arch::outb(PIT_MODE, 0x30);
        arch::outb(PIT_CH0, 0); // LSB
        arch::outb(PIT_CH0, 0); // MSB
    }

    /// Mode 4, software-triggered strobe: the counter runs down once per
    /// reload, so every event needs a set_next_event_tick() of its own.
    auto set_mode_oneshot() -> Status {
        Guard<sync::SpinLock, IrqSave> guard{m_lock};
        arch::outb(PIT_MODE, 0x38);
        return Status::Ok;
    }

    /// Mode 2, rate generator: the counter reloads PIT_LATCH by itself and
    /// interrupts every 1/HZ with no further programming.
    auto set_mode_periodic() -> Status {
        Guard<sync::SpinLock, IrqSave> guard{m_lock};
        arch::outb(PIT_MODE, 0x34);
        arch::outb(PIT_CH0, static_cast<u8>(PIT_LATCH & 0xffu));        // LSB
        arch::outb(PIT_CH0, static_cast<u8>((PIT_LATCH >> 8) & 0xffu)); // MSB
        return Status::Ok;
    }

    auto set_next_event_tick(Ticks delta) -> Status {
        Guard<sync::SpinLock, IrqSave> guard{m_lock};
        arch::outb(PIT_CH0, static_cast<u8>(delta & 0xffu));        // LSB
        arch::outb(PIT_CH0, static_cast<u8>((delta >> 8) & 0xffu)); // MSB
        return Status::Ok;
    }

    auto device() -> ClockEventDevice * {
        return &m_clkevt;
    }

  private:
    /// Serialises the multi-byte programming sequences: channel 0 takes its
    /// reload value as two consecutive writes, and a second writer landing
    /// between them leaves the counter holding half of each.
    ///
    /// Always taken with IrqSave. set_next_event_tick() runs from the tick
    /// interrupt, which reprograms the next one-shot event through the timer
    /// queue, as well as from thread context whenever a timer is armed; the
    /// mode changes run in thread context when the tick device is set up. A
    /// thread holding the lock with interrupts on would meet that interrupt
    /// spinning on it from the same CPU.
    sync::SpinLock m_lock;

    ClockEventDevice m_clkevt;
};

static ustl::LazyInit<I8253ClockDevice> I8253_CLKEVT_STORAGE;
ClockEventDevice *I8253_CLKEVT;

INIT_CODE
auto i8253_init() -> void {
    I8253_CLKEVT_STORAGE.init();
    I8253_CLKEVT = I8253_CLKEVT_STORAGE->device();

    ClockEventDevice::enroll(*I8253_CLKEVT, PIT_FREQUENCY, 0xF, 0x7FFF);
}

} // namespace ours
