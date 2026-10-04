#include <ours/arch/apic.hpp>
#include <ours/arch/time.hpp>
#include <ours/assert.hpp>
#include <ours/driver/i8253.hpp>
#include <ours/irq/irq_observer.hpp>
#include <ours/irq/mod.hpp>
#include <ours/platform/isa.hpp>
#include <ours/time/clock_event_device.hpp>

#include <atomic>

#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>

namespace ours::time {
namespace {
    /// The legacy timer line, ISA 0, and the clock-event device it drives:
    /// Linux's `global_clock_event`.
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

    auto timer_interrupt(VIrqNum, void *context) -> irq::IrqReturn {
        auto const *const timer = static_cast<LegacyTimer const *>(context);
        auto *const device = timer->device();
        DEBUG_ASSERT(device);

        device->handle_event();
        return irq::IrqReturn::Handled;
    }

    /// The line's observer, owned here for the kernel's lifetime: the line is
    /// Permanent, so nothing ever detaches it.
    // NOLINTNEXTLINE(readability-identifier-naming): UPPER_CASE, as above.
    constinit irq::IrqObserver LEGACY_TIMER_OBSERVER{&timer_interrupt, &LEGACY_TIMER,
                                                     irq::IrqFlags::Permanent, "IRQ-Timer"};
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

/// Two different numbers for the same interrupt, deliberately.
///
/// The IO-APIC's redirection table is indexed by GSI, so masking and
/// unmasking take apic_io_isa_to_global(ISA_IRQ_PIT) -- IRQ 0 is commonly
/// remapped to GSI 2. But platform_init_apic() programs that entry to deliver
/// vector `PlatformIrqMin + isa_irq`, derived from the *ISA* number, and
/// faults.cpp turns the vector back into an IRQ number by subtracting the
/// same base. So what reaches irq::handle_irq_generic() is the ISA number,
/// and that is what irq::attach() must name.
///
/// This reads like a mismatch and is not one; it was reported as a bug in the
/// kernel/time review and turned out to be correct, hence this comment.
auto arch_time_init() -> void {
    setup_pit();

    auto const status = irq::attach(ISA_IRQ_PIT, LEGACY_TIMER_OBSERVER);
    if (Status::Ok != status) {
        log::trace("Failed to register legacy timer interrupt: {}", static_cast<u32>(status));
    }

    apic_unmask_irq(apic_io_isa_to_global(ISA_IRQ_PIT));
}
GKTL_INIT_HOOK(InitTimerDevice, arch_time_init, gktl::InitLevel::Irq + 1);

} // namespace ours::time
