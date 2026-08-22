/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/phys/acpi.hpp>

namespace ours::phys {
namespace {
    /// //kernel/phys runs identity mapped, so every physical address is already
    /// readable at the same value.  The mapping is the whole address space and
    /// outlives everything, which satisfies acpi::PhysMapper's contract.
    struct IdentityMapper final: public acpi::PhysMapper {
        auto map(PhysAddr addr, usize size) -> ktl::Result<void const *> override {
            return ktl::ok(reinterpret_cast<void const *>(addr));
        }
    };

    IdentityMapper s_mapper;
} // namespace

auto make_acpi_table_set(PhysAddr rsdp) -> ktl::Result<acpi::TableSet> {
    return acpi::TableSet::from_rsdp(s_mapper, rsdp);
}

} // namespace ours::phys
