#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ours/arch/apic.hpp>
#include <ours/arch/time.hpp>
#include <ours/assert.hpp>
#include <ours/driver/i8253.hpp>
#include <ours/irq/mod.hpp>
#include <ours/platform/isa.hpp>
#include <ours/time/clock_event_device.hpp>

namespace ours::time {
ClockEventDevice *CLOCK_EVENT;

/// The legacy timer line's handler.
///
/// CLOCK_EVENT names whichever device currently owns this interrupt: the PIT
/// at first, then the HPET once hpet_init() enables the legacy replacement
/// route, which takes IRQ0 away from the PIT. It has to agree with what
/// TickDevice bound, or the interrupt arrives at a device with no handler and
/// the tick stops -- that is why the HPET's rating is set above the PIT's
/// (see HPET_CLKEVT_RATING).
auto timer_interrupt(VIrqNum irqnum, void *) -> irq::IrqReturn {
    DEBUG_ASSERT(CLOCK_EVENT);

    CLOCK_EVENT->handle_event();
    return irq::IrqReturn::Handled;
}

auto setup_pit() -> void {
    i8253_init();
    if (!CLOCK_EVENT) {
        CLOCK_EVENT = I8253_CLKEVT;
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
/// and that is what request_irq() must name.
///
/// This reads like a mismatch and is not one; it was reported as a bug in the
/// kernel/time review and turned out to be correct, hence this comment.
auto arch_time_init() -> void {
    setup_pit();

    auto status =
        irq::request_irq(ISA_IRQ_PIT, timer_interrupt, irq::IrqFlags::Permanent, "IRQ-Timer");
    if (Status::Ok != status) {
        log::trace("Failed to register legacy timer interrupt: {}", static_cast<u32>(status));
    }

    apic_unmask_irq(apic_io_isa_to_global(ISA_IRQ_PIT));
}
GKTL_INIT_HOOK(InitTimerDevice, arch_time_init, gktl::InitLevel::Irq + 1);

} // namespace ours::time
