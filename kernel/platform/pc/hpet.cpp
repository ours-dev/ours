#include <ours/arch/hpet.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/platform/acpi.hpp>
#include <ours/platform/mmio.hpp>

#include <acpi/structures/hpet.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>

namespace ours {
static auto platform_hpet_init() -> void {
    auto tables = get_acpi_tables();
    DEBUG_ASSERT(tables);
    auto const *hpet = tables->find<acpi::HpetTable>();

    if (!hpet) {
        log::info("No HPET table found.");
        return;
    }

    if (acpi::AddressSpace(hpet->address.address_space_id) != acpi::AddressSpace::SystemMemory) {
        log::info("HPET: MMIO unsupported.");
        return;
    }

    // The kernel maps the page the HPET's registers are in; no MmIo resource
    // grants it.
    platform::reserve_kernel_mmio(hpet->address.address, PAGE_SIZE);

    auto const status = hpet_init(hpet->address.address);
    if (Status::Ok != status) {
        // There is no to_string(Status) in the tree; log the raw value.
        log::info("Failed to initialize HPET: {}", u32(status));
    }
}
GKTL_INIT_HOOK(PlatformHpetInit, platform_hpet_init, gktl::InitLevel::Irq + 2);
} // namespace ours