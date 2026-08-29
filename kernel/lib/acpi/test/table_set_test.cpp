/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// TableSet: RSDP validation, root table selection, lookup, and the entry
/// range's behaviour on malformed input.

#include "acpi_test_blob.hpp"

#include <cstdlib>

namespace {

using acpi::test::Machine;
using acpi::test::require;

/// Builds a MADT with one I/O APIC, two local APICs and one source override.
auto add_madt(Machine &machine) -> void {
    auto const length = sizeof(acpi::madt::Table) + sizeof(acpi::madt::IoApicEntry) +
                        2 * sizeof(acpi::madt::LocalApicEntry) +
                        sizeof(acpi::madt::InterruptSourceOverrideEntry);
    auto *madt = machine.add_table<acpi::madt::Table>(acpi::madt::Table::kSignature, length);
    madt->lapic_addr = 0xfee0'0000;
    madt->flags = 1;

    auto *cursor = reinterpret_cast<u8 *>(madt) + sizeof(acpi::madt::Table);

    auto *io_apic = reinterpret_cast<acpi::madt::IoApicEntry *>(cursor);
    io_apic->header.type = u8(acpi::madt::Type::IoApic);
    io_apic->header.length = sizeof(acpi::madt::IoApicEntry);
    io_apic->io_apic_id = 7;
    io_apic->io_apic_address = 0xfec0'0000;
    io_apic->global_irq_base = 0;
    cursor += sizeof(acpi::madt::IoApicEntry);

    for (auto i = 0; i < 2; ++i) {
        auto *lapic = reinterpret_cast<acpi::madt::LocalApicEntry *>(cursor);
        lapic->header.type = u8(acpi::madt::Type::LocalApic);
        lapic->header.length = sizeof(acpi::madt::LocalApicEntry);
        lapic->acpi_processor_uid = u8(i);
        lapic->apic_id = u8(0x10 + i);
        lapic->flags = acpi::madt::LocalApicFlag::Enabled;
        cursor += sizeof(acpi::madt::LocalApicEntry);
    }

    auto *override_entry = reinterpret_cast<acpi::madt::InterruptSourceOverrideEntry *>(cursor);
    override_entry->header.type = u8(acpi::madt::Type::InterruptSourceOverride);
    override_entry->header.length = sizeof(acpi::madt::InterruptSourceOverrideEntry);
    override_entry->bus = 0;
    override_entry->source = 9;
    override_entry->global_irq = 9;
    // ActiveLow (0b11) polarity with Level (0b11) trigger.
    override_entry->flags = 0b1111;
}

auto test_lookup_and_entries() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish();

    auto &mapper = machine.mapper();
    auto tables = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(tables));
    require(tables->num_tables() == 1);

    auto const *madt = tables->find<acpi::madt::Table>();
    require(madt != nullptr);
    require(madt->lapic_addr == 0xfee0'0000);

    usize io_apics = 0;
    for (auto const &entry: acpi::entries<acpi::madt::IoApicEntry>(*madt)) {
        require(entry.io_apic_id == 7);
        require(entry.io_apic_address == 0xfec0'0000);
        ++io_apics;
    }
    require(io_apics == 1);

    usize local_apics = 0;
    for (auto const &entry: acpi::entries<acpi::madt::LocalApicEntry>(*madt)) {
        require(entry.apic_id == u8(0x10 + local_apics));
        ++local_apics;
    }
    require(local_apics == 2);

    // A type nothing in the table uses yields an empty, non-malformed range.
    auto empty = acpi::entries<acpi::madt::LocalX2ApicEntry>(*madt);
    require(empty.begin() == empty.end());
    require(!empty.malformed());

    // The range is a range: `break` works, which the old callback interface
    // could not express -- it declared a Status return and then ignored it.
    usize visited = 0;
    for ([[maybe_unused]]
         auto const &entry: acpi::entries<acpi::madt::LocalApicEntry>(*madt)) {
        ++visited;
        break;
    }
    require(visited == 1);
}

/// The shape //kernel/platform/pc/interrupt.cpp consumes.  That file cannot be
/// compiled in this tree for reasons unrelated to ACPI (it includes a
/// nonexistent arch/x86/pic.hpp), so its decoding path is pinned here instead.
auto test_interrupt_source_override_decoding() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish();

    auto &mapper = machine.mapper();
    auto tables = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(tables));
    auto const *madt = tables->find<acpi::madt::Table>();
    require(madt != nullptr);

    usize overrides = 0;
    for (auto const &entry: acpi::entries<acpi::madt::InterruptSourceOverrideEntry>(*madt)) {
        require(entry.source == 9);
        require(entry.global_irq == 9);
        // The old definition declared these flags as a u8, which made the entry
        // 9 bytes instead of 10.  It happened to decode on little-endian
        // targets because both fields live in the low byte.
        require(entry.polarity() == acpi::madt::Polarity::ActiveLow);
        require(entry.trigger_mode() == acpi::madt::TriggerMode::Level);
        ++overrides;
    }
    require(overrides == 1);
}

auto test_lookup_is_memoised() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish();

    auto &mapper = machine.mapper();
    auto tables = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(tables));

    require(tables->find<acpi::madt::Table>() != nullptr);
    auto const after_first = mapper.calls;
    for (auto i = 0; i < 16; ++i) {
        require(tables->find<acpi::madt::Table>() != nullptr);
    }
    // Repeat lookups must not walk the root table again.  The old parser
    // re-scanned and re-mapped on every single call.
    require(mapper.calls == after_first);

    // Negative results are memoised too.
    require(tables->find<acpi::srat::Table>() == nullptr);
    auto const after_miss = mapper.calls;
    require(tables->find<acpi::srat::Table>() == nullptr);
    require(mapper.calls == after_miss);
}

auto test_rsdp_signature_must_match_both_halves() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish();

    auto &mapper = machine.mapper();
    require(bool(acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys())));

    // "RSD " in the first half and garbage in the second used to pass, because
    // the two halves were combined with || instead of &&.
    machine.rsdp()->v1.sig_high = acpi::Signature::from_chars("XXXX");
    machine.seal_rsdp();
    require(!acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys()));

    machine.rsdp()->v1.sig_high = acpi::sdt::Rsdp::kSignatureHigh;
    machine.rsdp()->v1.sig_low = acpi::Signature::from_chars("XXXX");
    machine.seal_rsdp();
    require(!acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys()));

    machine.rsdp()->v1.sig_low = acpi::sdt::Rsdp::kSignatureLow;
    machine.seal_rsdp();
    require(bool(acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys())));
}

auto test_rsdp_checksum() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish();

    auto &mapper = machine.mapper();
    machine.rsdp()->v1.checksum = u8(machine.rsdp()->v1.checksum + 1);
    require(!acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys()));
}

auto test_falls_back_to_rsdt() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish();

    auto &mapper = machine.mapper();

    // A corrupted XSDT must fall back to the RSDT rather than fail outright.
    machine.xsdt()->header.checksum = u8(machine.xsdt()->header.checksum + 1);
    auto tables = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(tables));
    require(tables->num_tables() == 1);
    require(tables->find<acpi::madt::Table>() != nullptr);

    // So must an XSDT whose payload is not a whole number of 64-bit pointers.
    machine.reseal_xsdt();
    machine.xsdt()->header.length += 1;
    machine.reseal_xsdt();
    auto ragged = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(ragged));
    require(ragged->num_tables() == 1);
}

auto test_revision_one_uses_rsdt() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish(/*revision=*/0);

    auto &mapper = machine.mapper();
    auto tables = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(tables));
    require(tables->find<acpi::madt::Table>() != nullptr);
}

auto test_table_shorter_than_struct_is_rejected() -> void {
    Machine machine;
    add_madt(machine);
    // A SLIT that stops before its own locality_count field.  find<> must
    // refuse it: reading locality_count out of this table would run past the
    // 40 bytes that were mapped.
    auto const short_len = sizeof(acpi::slit::Table) - 4;
    machine.add_table<acpi::slit::Table>(acpi::slit::Table::kSignature, short_len);
    machine.finish();

    auto &mapper = machine.mapper();
    auto tables = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(tables));
    require(tables->num_tables() == 2);
    require(tables->find<acpi::slit::Table>() == nullptr);
    // The healthy table alongside it is still reachable.
    require(tables->find<acpi::madt::Table>() != nullptr);
}

auto test_bad_table_checksum_is_skipped() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish();
    machine.table_bytes(0)[9] = u8(machine.table_bytes(0)[9] + 1);

    auto &mapper = machine.mapper();
    auto tables = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(tables));
    require(tables->find<acpi::madt::Table>() == nullptr);
}

auto test_entry_walk_stops_on_lying_length() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish();

    auto &mapper = machine.mapper();
    auto tables = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(tables));
    auto *madt = const_cast<acpi::madt::Table *>(tables->find<acpi::madt::Table>());
    require(madt != nullptr);

    // An entry claiming to be longer than the table's remaining bytes.
    auto *first = reinterpret_cast<acpi::EntryHeader *>(reinterpret_cast<u8 *>(madt) +
                                                        sizeof(acpi::madt::Table));
    auto const saved = first->length;
    first->length = 0xff;

    auto range = acpi::entries<acpi::madt::LocalApicEntry>(*madt);
    usize seen = 0;
    for ([[maybe_unused]]
         auto const &entry: range) {
        ++seen;
    }
    require(seen == 0);
    require(range.malformed());

    // A zero length entry must not spin forever.
    first->length = 0;
    auto zero_range = acpi::entries<acpi::madt::LocalApicEntry>(*madt);
    seen = 0;
    for ([[maybe_unused]]
         auto const &entry: zero_range) {
        ++seen;
    }
    require(seen == 0);
    require(zero_range.malformed());

    first->length = saved;
    auto healthy = acpi::entries<acpi::madt::LocalApicEntry>(*madt);
    seen = 0;
    for ([[maybe_unused]]
         auto const &entry: healthy) {
        ++seen;
    }
    require(seen == 2);
    require(!healthy.malformed());
}

auto test_mapper_failure_is_propagated() -> void {
    Machine machine;
    add_madt(machine);
    machine.finish();

    auto &mapper = machine.mapper();
    mapper.fail_all = true;
    require(!acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys()));
}

} // namespace

auto main() -> int {
    test_lookup_and_entries();
    test_interrupt_source_override_decoding();
    test_lookup_is_memoised();
    test_rsdp_signature_must_match_both_halves();
    test_rsdp_checksum();
    test_falls_back_to_rsdt();
    test_revision_one_uses_rsdt();
    test_table_shorter_than_struct_is_rejected();
    test_bad_table_checksum_is_skipped();
    test_entry_walk_stops_on_lying_length();
    test_mapper_failure_is_propagated();
    return 0;
}
