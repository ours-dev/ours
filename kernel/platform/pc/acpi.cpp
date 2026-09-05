/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/log.hpp>
#include <ours/assert.hpp>
#include <ours/boot/handoff.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/platform/acpi.hpp>
#include <ours/platform/init.hpp>
#include <ustl/lazy_init.hpp>
#include <ustl/util/move.hpp>

namespace ours {
FORCE_USED
acpi::TableSet *ACPI_TABLES = nullptr;

namespace {
    FORCE_USED
    ustl::LazyInit<acpi::TableSet> ACPI_TABLES_STORAGE;

    /// The physmap covers all of usable physical memory with a permanent linear
    /// mapping, so a successful translation is readable for its whole length
    /// and never unmapped -- both halves of acpi::PhysMapper's contract.
    struct PhysMapMapper final: public acpi::PhysMapper {
        auto map(PhysAddr addr, usize size) -> ktl::Result<void const *> override {
            if (auto ptr = mm::PhysMap::phys_to_virt<void const>(addr)) {
                return ktl::ok(ptr);
            }

            return ktl::err(Status::InternalError);
        }
    };

    PhysMapMapper MAPPER;

    FORCE_USED INIT_CODE
    auto platform_init_acpi(PhysAddr rsdp) -> void {
        DEBUG_ASSERT(!ACPI_TABLES, "Duplicated calls");
        if (auto tables = acpi::TableSet::from_rsdp(MAPPER, rsdp)) {
            ACPI_TABLES_STORAGE.init(ustl::move(tables.unwrap()));
            ACPI_TABLES = ACPI_TABLES_STORAGE.data();
        }
    }

    FORCE_USED INIT_CODE
    auto platform_init_acpi() -> void {
        // global_handoff() lives in //kernel/phys and is not reachable from the
        // kernel proper; ours::handoff() is the accessor on this side.
        //
        // acpi_rsdp is an Option: phys only fills it in when it actually found
        // an RSDP, so absence is a legitimate state (no ACPI on this machine),
        // not an error. Leave ACPI_TABLES null and let consumers see that.
        auto const &rsdp = boot::handoff().acpi_rsdp;
        if (!rsdp.is_some()) {
            log::warn("No ACPI RSDP was handed off; skipping ACPI table setup");
            return;
        }

        platform_init_acpi(rsdp.value());
    }
    GKTL_INIT_HOOK(AcpiInit, platform_init_acpi, PlatformInitLevel::StartUp);
} // namespace

} // namespace ours
