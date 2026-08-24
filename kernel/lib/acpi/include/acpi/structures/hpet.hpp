/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_STRUCTURES_HPET_HPP
#define ACPI_STRUCTURES_HPET_HPP 1

#include <acpi/structures/header.hpp>

namespace acpi {
/// High precision event timer table.
///
/// Reference: IA-PC HPET Specification 1.0a, Section 3.2.4.
///
/// This header describes the ACPI *table* only.  The HPET's memory mapped
/// register block used to live here too, which dragged ustl/bitfields.hpp and
/// ours/assert.hpp into what is otherwise a plain layout header; it now lives
/// with its only consumer, in //kernel/arch/x86/kernel:headers
/// (ours/arch/hpet-regs.hpp).
struct PACKED HpetTable {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return header.length;
    }

    TableHeader header;
    u32 event_timer_block_id;
    GenericAddress address;
    u8 hpet_number;
    u16 minimum_tick;
    u8 page_protection;

    constexpr static Signature kSignature = Signature::from_chars("HPET");
};
static_assert(sizeof(HpetTable) == 56);

} // namespace acpi

#endif // #ifndef ACPI_STRUCTURES_HPET_HPP
