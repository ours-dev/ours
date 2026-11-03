#include <ours/arch/aspace_layout.hpp>
#include <ours/irq/init.hpp>

#include <arch/interrupt.hpp>

namespace ours::irq {
// The line table is built by the platform, with register_domain(), and not here:
// which lines exist is known only once the platform has read its interrupt
// controllers from the firmware, on x86 the MADT at InitLevel::Irq.

auto init() -> void {
    // arch::local_irq_enable();
}

} // namespace ours::irq
