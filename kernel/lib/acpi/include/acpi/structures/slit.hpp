/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_STRUCTURES_SLIT_HPP
#define ACPI_STRUCTURES_SLIT_HPP 1

#include <acpi/structures/header.hpp>

namespace acpi::slit {
/// System locality distance information table.
///
/// Reference: ACPI v6.5 Section 5.2.17.
///
/// The payload is a `locality_count` squared matrix of one byte distances.
/// Nothing here validates that the declared count actually fits in the table --
/// that check belongs to `acpi::numa::DistanceMatrix`, which is the only
/// supported way to read `entry`.
struct PACKED Table {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return header.length;
    }

    TableHeader header;
    u64 locality_count;
    u8 entry[];

    constexpr static Signature SIGNATURE = Signature::from_chars("SLIT");
};
static_assert(sizeof(Table) == 44);

} // namespace acpi::slit

#endif // #ifndef ACPI_STRUCTURES_SLIT_HPP
