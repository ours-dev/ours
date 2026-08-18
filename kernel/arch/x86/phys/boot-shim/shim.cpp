#include <ours/panic.hpp>
#include <ours/phys/acpi.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/console.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/print.hpp>

#include "../legacy-boot.hpp"

#include <ccrt/runtime.hpp>

namespace ours::phys {
NO_MANGLE NO_RETURN 
auto phys_main(PhysAddr loader_param) -> void {
    ccrt::init_global_static_objects();
    init_early_console();
    println("loader params at 0x{:X}", loader_param);

    auto &legacy = LegacyBoot::get();
    auto const handoff_status = legacy.parse_params(loader_param);
    if (handoff_status != Status::Ok) {
        panic("Invalid Multiboot2 handoff: {}", static_cast<u32>(handoff_status));
    }

    auto handoff = global_handoff();
    handoff->acpi_rsdp = legacy.acpi_rsdp;
    auto const ramdisk = legacy.ramdisk;
    obi_main(PhysAddr(ramdisk.data()), ramdisk.size());

    panic("Never reach at here");
}

} // namespace ours::phys
