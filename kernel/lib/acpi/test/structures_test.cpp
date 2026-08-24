/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Layout tests for the ACPI structure definitions.
///
/// Most of these are static_asserts inside the headers themselves; what this
/// file adds is field *offsets* and the type numbering, which is where the
/// previous definitions went wrong without changing any struct's size.

#include <acpi/signature.hpp>
#include <acpi/structures/dbg2.hpp>
#include <acpi/structures/hpet.hpp>
#include <acpi/structures/madt.hpp>
#include <acpi/structures/root.hpp>
#include <acpi/structures/slit.hpp>
#include <acpi/structures/srat.hpp>

#include <cstdlib>

namespace {

auto require(bool condition) -> void {
    if (!condition) {
        std::abort();
    }
}

// ACPI 6.5 Table 5.20.  The old enum skipped LocalApicAddressOverride and
// invented an IoX2Apic, which slid types 0x05..0x09 one place each.
static_assert(u8(acpi::madt::Type::LocalApic) == 0x00);
static_assert(u8(acpi::madt::Type::IoApic) == 0x01);
static_assert(u8(acpi::madt::Type::InterruptSourceOverride) == 0x02);
static_assert(u8(acpi::madt::Type::NmiSource) == 0x03);
static_assert(u8(acpi::madt::Type::LocalApicNmi) == 0x04);
static_assert(u8(acpi::madt::Type::LocalApicAddressOverride) == 0x05);
static_assert(u8(acpi::madt::Type::IoSapic) == 0x06);
static_assert(u8(acpi::madt::Type::LocalSapic) == 0x07);
static_assert(u8(acpi::madt::Type::PlatformInterruptSources) == 0x08);
static_assert(u8(acpi::madt::Type::LocalX2Apic) == 0x09);
static_assert(u8(acpi::madt::Type::LocalX2ApicNmi) == 0x0A);
static_assert(u8(acpi::madt::Type::GicCpuInterface) == 0x0B);
static_assert(u8(acpi::madt::Type::GicInterruptTranslationService) == 0x0F);
static_assert(u8(acpi::madt::Type::MultiprocessorWakeup) == 0x10);

// Entry lengths, ACPI 6.5 Section 5.2.12.
static_assert(sizeof(acpi::madt::LocalApicEntry) == 8);
static_assert(sizeof(acpi::madt::IoApicEntry) == 12);
static_assert(sizeof(acpi::madt::InterruptSourceOverrideEntry) == 10);
static_assert(sizeof(acpi::madt::NmiSourceEntry) == 8);
static_assert(sizeof(acpi::madt::LocalApicNmiEntry) == 6);
static_assert(sizeof(acpi::madt::LocalApicAddressOverrideEntry) == 12);
static_assert(sizeof(acpi::madt::IoSapicEntry) == 16);
static_assert(sizeof(acpi::madt::LocalSapicEntry) == 16);
static_assert(sizeof(acpi::madt::PlatformInterruptSourcesEntry) == 16);
static_assert(sizeof(acpi::madt::LocalX2ApicEntry) == 16);
static_assert(sizeof(acpi::madt::LocalX2ApicNmiEntry) == 12);

// Field offsets that a wrong-but-same-size definition would still get wrong.
static_assert(offsetof(acpi::madt::InterruptSourceOverrideEntry, global_irq) == 4);
static_assert(offsetof(acpi::madt::InterruptSourceOverrideEntry, flags) == 8);
static_assert(offsetof(acpi::madt::NmiSourceEntry, flags) == 2);
static_assert(offsetof(acpi::madt::NmiSourceEntry, global_irq) == 4);
static_assert(offsetof(acpi::madt::LocalX2ApicEntry, x2apic_id) == 4);
static_assert(offsetof(acpi::madt::LocalX2ApicEntry, flags) == 8);
static_assert(offsetof(acpi::madt::LocalX2ApicEntry, acpi_processor_uid) == 12);
static_assert(offsetof(acpi::madt::LocalX2ApicNmiEntry, acpi_processor_uid) == 4);

static_assert(sizeof(acpi::TableHeader) == 36);
static_assert(offsetof(acpi::TableHeader, length) == 4);
static_assert(offsetof(acpi::TableHeader, oem_table_id) == 16);
static_assert(sizeof(acpi::GenericAddress) == 12);
static_assert(offsetof(acpi::GenericAddress, address) == 4);

static_assert(sizeof(acpi::sdt::Rsdp) == 20);
static_assert(sizeof(acpi::sdt::RsdpV2) == 36);
static_assert(offsetof(acpi::sdt::RsdpV2, xsdt_addr) == 24);
static_assert(sizeof(acpi::sdt::Rsdt) == 36);
static_assert(sizeof(acpi::sdt::Xsdt) == 36);

static_assert(sizeof(acpi::srat::Table) == 48);
static_assert(sizeof(acpi::srat::ProcessorAffinityEntry) == 16);
static_assert(sizeof(acpi::srat::MemoryAffinityEntry) == 40);
static_assert(sizeof(acpi::srat::ProcessorX2ApicAffinityEntry) == 24);
static_assert(sizeof(acpi::slit::Table) == 44);
static_assert(sizeof(acpi::dbg2::Table) == 44);
static_assert(sizeof(acpi::dbg2::Device) == 22);
static_assert(sizeof(acpi::HpetTable) == 56);

// Signatures are little-endian character order, and the type stays POD so that
// it can sit inside a PACKED table without clang refusing to pack it.
static_assert(acpi::Signature::from_chars("APIC").value == 0x43495041);
static_assert(acpi::Signature::from_chars("RSD ").value == 0x20445352);
static_assert(acpi::Signature::from_chars("XSDT") == acpi::Signature::from_raw(0x54445358));
static_assert(acpi::Signature::from_chars("APIC") != acpi::Signature::from_chars("SRAT"));
static_assert(std::is_trivial_v<acpi::Signature>);
static_assert(std::is_standard_layout_v<acpi::Signature>);

} // namespace

auto main() -> int {
    // The MPS INTI fields are two-bit values, not independent bits.  Masking
    // them by hand -- as the old madt::Flag enum invited -- makes ActiveLow
    // (0b11) test true for ActiveHigh (0b01).
    require(acpi::madt::polarity_of(0b0000) == acpi::madt::Polarity::Conforms);
    require(acpi::madt::polarity_of(0b0001) == acpi::madt::Polarity::ActiveHigh);
    require(acpi::madt::polarity_of(0b0011) == acpi::madt::Polarity::ActiveLow);
    require(acpi::madt::trigger_mode_of(0b0000) == acpi::madt::TriggerMode::Conforms);
    require(acpi::madt::trigger_mode_of(0b0100) == acpi::madt::TriggerMode::Edge);
    require(acpi::madt::trigger_mode_of(0b1100) == acpi::madt::TriggerMode::Level);
    // Polarity ActiveLow with trigger Edge must decode both fields independently.
    require(acpi::madt::polarity_of(0b0111) == acpi::madt::Polarity::ActiveLow);
    require(acpi::madt::trigger_mode_of(0b0111) == acpi::madt::TriggerMode::Edge);

    // SRAT proximity domains are split across two non-adjacent fields.
    acpi::srat::ProcessorAffinityEntry processor{};
    processor.proximity_domain_low = 0x21;
    processor.proximity_domain_high[0] = 0x43;
    processor.proximity_domain_high[1] = 0x65;
    processor.proximity_domain_high[2] = 0x87;
    require(processor.proximity_domain() == 0x87654321);

    acpi::srat::MemoryAffinityEntry memory{};
    memory.base_address_low = 0x1000;
    memory.base_address_high = 0x2;
    memory.length_low = 0x3000;
    memory.length_high = 0x4;
    require(memory.base_address() == 0x2'0000'1000ull);
    require(memory.length() == 0x4'0000'3000ull);
    require(!memory.enabled());
    memory.flags = acpi::srat::Flag::Enabled;
    require(memory.enabled());

    auto const sign = acpi::Signature::from_chars("SRAT");
    require(sign.name() == "SRAT");

    return 0;
}
