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

auto timer_interrupt(VIrqNum irqnum, void *) -> irq::IrqReturn {
    DEBUG_ASSERT(CLOCK_EVENT);

    CLOCK_EVENT->handle_event();
    return irq::IrqReturn::Handled;
}

auto setup_pit() -> void {
    // By default we make PIT working on the periodic mode.
    i8253_init(false);
    if (!CLOCK_EVENT) {
        CLOCK_EVENT = I8253_CLKEVT;
    }
}

auto arch_time_init() -> void {
    setup_pit();

    auto const irqnum = apic_io_isa_to_global(ISA_IRQ_PIT);
    auto status = irq::request_irq(0, timer_interrupt, irq::IrqFlags::Permanent, "IRQ-Timer");
    if (Status::Ok != status) {
        log::trace("Failed to register legacy timer interrupt: {}", static_cast<u32>(status));
    }

    apic_unmask_irq(irqnum);
}
GKTL_INIT_HOOK(InitTimerDevice, arch_time_init, gktl::InitLevel::Irq + 1);

} // namespace ours::time
