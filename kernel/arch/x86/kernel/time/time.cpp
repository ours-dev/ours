#include <ours/arch/apic.hpp>
#include <ours/arch/time.hpp>
#include <ours/assert.hpp>
#include <ours/driver/i8253.hpp>
#include <ours/irq/mod.hpp>
#include <ours/irq/registration.hpp>
#include <ours/platform/isa.hpp>
#include <ours/time/clock_event_device.hpp>

#include <atomic>

#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>

namespace ours::time {
namespace {
    /// The legacy timer line, ISA 0, and the clock-event device it drives.
    ///
    /// The device changes once. The PIT drives the line at first, then the
    /// HPET, once hpet_init() enables the legacy replacement route, which
    /// takes IRQ0 away from the PIT. It has to agree with what TickDevice
    /// bound, or the interrupt arrives at a device with no handler and the
    /// tick stops -- that is why the HPET's rating is set above the PIT's
    /// (see HPET_CLKEVT_RATING).
    ///
    /// The line's handler is handed this object as its context and reads the
    /// device from it, instead of a global. The hand-over is written in
    /// thread context and read by the handler, which can run at any moment,
    /// so the pointer is atomic: release on the store and acquire on the
    /// load, so that a handler that sees the HPET's device also sees the
    /// writes that enrolled it.
    class LegacyTimer final {
      public:
        [[nodiscard]]
        auto device() const -> ClockEventDevice * {
            return m_device.load(std::memory_order_acquire);
        }

        auto set_device(ClockEventDevice &device) -> void {
            m_device.store(&device, std::memory_order_release);
        }

      private:
        std::atomic<ClockEventDevice *> m_device = nullptr;
    };

    // Namespace-scope objects are UPPER_CASE throughout this tree; .clang-tidy's
    // lower_case rule predates that.
    // NOLINTNEXTLINE(readability-identifier-naming)
    constinit LegacyTimer LEGACY_TIMER;

    auto timer_interrupt(IrqId, void *context) -> irq::IrqReturn {
        auto const *const timer = static_cast<LegacyTimer const *>(context);
        auto *const device = timer->device();
        DEBUG_ASSERT(device);

        device->handle_event();
        return irq::IrqReturn::Handled;
    }

    /// The line's action, owned here for the kernel's lifetime: the line is
    /// Permanent, so nothing ever detaches it.
    // NOLINTNEXTLINE(readability-identifier-naming): UPPER_CASE, as above.
    constinit irq::Registration LEGACY_TIMER_ACTION{
        {.handle = &timer_interrupt}, &LEGACY_TIMER, irq::IrqFlags::Permanent, "IRQ-Timer"};
} // namespace

auto global_clock_event() -> ClockEventDevice * {
    return LEGACY_TIMER.device();
}

auto set_global_clock_event(ClockEventDevice &device) -> void {
    LEGACY_TIMER.set_device(device);
}

auto setup_pit() -> void {
    i8253_init();
    if (!LEGACY_TIMER.device()) {
        LEGACY_TIMER.set_device(*I8253_CLKEVT);
    }
}

/// The line is named by its virq, like every line: ISA 0 is commonly sent to
/// GSI 2, and the line made for that GSI is the one both the attach and the
/// unmask take.
///
/// A line whose attach failed stays masked, and the failure is an error:
/// unmasked with nobody on it, the tick would arrive at an empty line and be
/// counted as spurious, and the kernel would run on without a tick and
/// without saying so.
auto arch_time_init() -> void {
    setup_pit();

    auto const line = platform::isa_irq_line(ISA_IRQ_PIT);
    auto const attached = irq::attach(line, LEGACY_TIMER_ACTION);
    if (Status::Ok != attached) {
        log::error("Failed to attach the legacy timer to line {}: {}", static_cast<u32>(line),
                   to_string(attached));
        return;
    }

    // The line was just attached, so it exists and has the IO-APIC's chip.
    [[maybe_unused]]
    auto const unmasked = irq::unmask(line);
    DEBUG_ASSERT(Status::Ok == unmasked, "unmasking line {}: {}", static_cast<u32>(line),
                 to_string(unmasked));
}
GKTL_INIT_HOOK(InitTimerDevice, arch_time_init, gktl::InitLevel::Irq + 1);

} // namespace ours::time
