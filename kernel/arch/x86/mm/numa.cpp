#include <ours/platform/acpi.hpp>
#include <ours/platform/init.hpp>
#include <ours/start.hpp>

namespace ours {
// This translation unit only participates in the build when a NUMA mode is
// selected (kernel/arch/x86/mm/BUILD.gn gates it on mm_numa ||
// mm_numa_emulation), so the macros from //kernel/products:memory are always
// defined here; the copy keeps the gate readable and honest about which mode
// pulled the file in.
static bool const NUMA_ENABLED = MM_NUMA || MM_NUMA_EMULATION;

WEAK_LINK auto pci_init_numa() -> Status {
    return Status::Unsupported;
}

WEAK_LINK auto acpi_init_numa() -> Status {
    return Status::Unsupported;
}

INIT_CODE static auto x86_init_numa() -> void {
    if (!NUMA_ENABLED) {
        return;
    }
    if (Status::Ok == acpi_init_numa()) {
        return;
    }
    if (Status::Ok == pci_init_numa()) {
        return;
    }
}

/// Before initializing the Physical Memory Manager (PMM), the system should gather
/// Non-Uniform Memory Access (NUMA) topology information if the architecture supports it.
/// Directly invoking NUMA initialization within `init_platform_early` would introduce
/// redundant preprocessor macros to conditionally enable/disable NUMA functionality.
/// Instead, encapsulating this logic within a modular initialization hook streamlines
/// the codebase by eliminating such macros, improving maintainability and reducing conditional
/// compilation overhead.
GKTL_INIT_HOOK(NumaInit, x86_init_numa, PlatformInitLevel::PrePmm + 1);

} // namespace ours