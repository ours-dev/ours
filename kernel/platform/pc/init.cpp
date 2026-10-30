#include <ours/arch/x86/mp.hpp>
#include <ours/cpu.hpp>
#include <ours/irq/init.hpp>
#include <ours/mm/init.hpp>
#include <ours/platform/acpi.hpp>
#include <ours/platform/init.hpp>

#include <arch/x86/pic.hpp>

namespace ours {
extern "C" INIT_CODE
auto init_platform_early() -> void {
    set_platform_init_level(PlatformInitLevel::StartUp);

    set_platform_init_level(PlatformInitLevel::PrePmm);

    platform_init_pmm();
    set_platform_init_level(PlatformInitLevel::Pmm);
}

FORCE_INLINE
static auto platform_init_mp() -> void {
    x86_bring_aps(Cpu::possible_mask());
}

extern "C" INIT_CODE
auto init_platform() -> void {
    platform_init_mp();
}

} // namespace ours