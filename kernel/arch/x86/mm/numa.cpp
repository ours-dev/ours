#include <ours/platform/acpi.hpp>
#include <ours/platform/init.hpp>
#include <ours/start.hpp>

#include <logz4/log.hpp>

namespace ours {
// This translation unit only participates in the build when a NUMA mode is
// selected (kernel/arch/x86/mm/BUILD.gn gates it on mm_numa ||
// mm_numa_emulation), so the macros from //kernel/products:memory are always
// defined here; the copy keeps the gate readable and honest about which mode
// pulled the file in.
static bool const NUMA_ENABLED = MM_NUMA || MM_NUMA_EMULATION;

WEAK_LINK
auto pci_init_numa() -> Status {
    return Status::Unsupported;
}

WEAK_LINK
auto acpi_init_numa() -> Status {
    return Status::Unsupported;
}

INIT_CODE
static auto x86_init_numa() -> void {
    if (!NUMA_ENABLED) {
        return;
    }

    auto const from_acpi = acpi_init_numa();
    if (Status::Ok == from_acpi) {
        return;
    }

    auto const from_pci = pci_init_numa();
    if (Status::Ok == from_pci) {
        return;
    }

    // Neither source gave us a topology. Both statuses are named because the
    // two fail for unrelated reasons and the interesting one is whichever was
    // expected to work.
    //
    // Said out loud because the boot that follows is otherwise
    // indistinguishable from a healthy one: PmNode::bootstrap() fails only
    // when no node owns memory at all, so whatever node set it does find goes
    // through create_nodes(), init() and connect_nodes() and dumps a
    // perfectly ordinary fallback order. This file used to test both statuses,
    // discard them, and include no logging header at all, so "this machine
    // declares no NUMA" and "the topology parse failed" produced byte-identical
    // output -- nothing -- and a correct one-node boot was filed as a kernel
    // bug on 2026-10-29 partly on the strength of that silence.
    //
    // What this does NOT say is how many nodes come up, because it cannot know
    // and the first draft of it got that wrong. acpi_init_numa() applies the
    // SRAT's memory affinities region by region, marking every domain it moves
    // as a possible node, and nothing rolls those marks back -- so a walk that
    // stops half way, bootmem refusing one range, arrives here with part of
    // the machine's nodes already registered. Claiming one node would print
    // the opposite of what happened. The node count is reported by
    // PmNode::bootstrap(), where it is a fact.
    //
    // The example that used to stand here was a machine with an SRAT and no
    // SLIT, which does not arrive any more: a missing or unreadable SLIT is a
    // topology without distances, not the absence of a topology, so
    // acpi_init_numa() now finishes the discovery and returns Ok for it. See
    // docs/changes/261029/mm.md.
    log::info("numa: no firmware topology (acpi {}, pci {})", to_string(from_acpi),
              to_string(from_pci));
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