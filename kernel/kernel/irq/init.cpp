#include <ours/irq/init.hpp>
#include <ours/irq/irq_dispatcher.hpp>
#include <ours/irq/mod.hpp>
#include <ours/arch/aspace_layout.hpp>

#include <arch/interrupt.hpp>

#include <gktl/init_hook.hpp>

namespace ours::irq {
    static auto initialize_early() -> void {
        // TODO(SmallHuaZi): Temporary solution for x86
        init_early(16);
    }
    GKTL_INIT_HOOK(Temporary, initialize_early, gktl::InitLevel::Vmm);

    auto init() -> void {
        // arch::local_irq_enable();
    }

} // namespace ours::irq
