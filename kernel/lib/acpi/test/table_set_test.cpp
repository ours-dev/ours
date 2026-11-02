/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// TableSet: RSDP validation, root table selection, lookup, and the entry
/// range's behaviour on malformed input.

#include <cstdlib>

#include "acpi_test_blob.hpp"

namespace {

using acpi::test::Machine;
using acpi::test::require;

/// Builds a MADT with one I/O APIC, two local APICs and one source override.
auto add_madt(Machine &machine) -> void {
    auto const length = sizeof(acpi::madt::Table) + sizeof(acpi::madt::IoApicEntry) +
                        2 * sizeof(acpi::madt::LocalApicEntry) +
                        sizeof(acpi::madt::InterruptSourceOverrideEntry);
    auto *madt = machine.add_table<acpi::madt::Table>(acpi::madt::Table::SIGNATURE, length);
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

    machine.rsdp()->v1.sig_high = acpi::sdt::Rsdp::SIGNATURE_HIGH;
    machine.rsdp()->v1.sig_low = acpi::Signature::from_chars("XXXX");
    machine.seal_rsdp();
    require(!acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys()));

    machine.rsdp()->v1.sig_low = acpi::sdt::Rsdp::SIGNATURE_LOW;
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
    machine.add_table<acpi::slit::Table>(acpi::slit::Table::SIGNATURE, short_len);
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

constexpr auto DSDT_SIGNATURE = acpi::Signature::from_chars("DSDT");

/// A table of `body` bytes after its header, every one `fill`.  Not listed by
/// the root table: the DSDT is not.
auto add_dsdt(Machine &machine, usize const body, u8 const fill) -> void {
    auto *dsdt = machine.add_unlisted_table<acpi::TableHeader>(DSDT_SIGNATURE,
                                                               sizeof(acpi::TableHeader) + body);
    std::memset(reinterpret_cast<u8 *>(dsdt) + sizeof(acpi::TableHeader), fill, body);
}

/// A FADT of `length` bytes whose pointers to the DSDT are the two given.
/// x_dsdt is written only if the FADT is long enough to have one.
auto add_fadt(Machine &machine, usize const length, u32 const dsdt, u64 const x_dsdt) -> void {
    // The pointer is written into the allocation, so it has to be inside it.
    require(length >= offsetof(acpi::Fadt, dsdt) + sizeof(u32));
    auto *fadt = machine.add_table<acpi::Fadt>(acpi::Fadt::SIGNATURE, length);
    fadt->dsdt = dsdt;
    if (length >= sizeof(acpi::Fadt)) {
        fadt->x_dsdt = x_dsdt;
    }
}

auto dsdt_of(Machine &machine) -> acpi::TableHeader const * {
    auto tables = acpi::TableSet::from_rsdp(machine.mapper(), machine.rsdp_phys());
    require(bool(tables));
    return tables->dsdt();
}

auto test_dsdt_is_found_through_the_fadt() -> void {
    Machine machine;
    add_dsdt(machine, 16, 0x5a);
    add_fadt(machine, sizeof(acpi::Fadt), u32(machine.unlisted_phys(0)), 0);
    machine.finish();

    // The root table lists the FADT and nothing else: the DSDT is out of its
    // reach, which is why find<> cannot be the way to it.
    auto tables = acpi::TableSet::from_rsdp(machine.mapper(), machine.rsdp_phys());
    require(bool(tables));
    require(tables->num_tables() == 1);

    auto const *dsdt = tables->dsdt();
    require(dsdt != nullptr);
    require(dsdt->sign == DSDT_SIGNATURE);
    require(dsdt->length == sizeof(acpi::TableHeader) + 16);
}

auto test_x_dsdt_wins_over_dsdt() -> void {
    // Two DSDTs of different lengths, the first behind the 32 bit pointer and
    // the second behind the 64 bit one.
    Machine both;
    add_dsdt(both, 16, 0x11);
    add_dsdt(both, 32, 0x22);
    add_fadt(both, sizeof(acpi::Fadt), u32(both.unlisted_phys(0)), both.unlisted_phys(1));
    both.finish();
    auto const *dsdt = dsdt_of(both);
    require(dsdt != nullptr);
    require(dsdt->length == sizeof(acpi::TableHeader) + 32);

    // x_dsdt of zero means there is none: the 32 bit pointer is used.
    Machine zero;
    add_dsdt(zero, 16, 0x11);
    add_dsdt(zero, 32, 0x22);
    add_fadt(zero, sizeof(acpi::Fadt), u32(zero.unlisted_phys(0)), 0);
    zero.finish();
    dsdt = dsdt_of(zero);
    require(dsdt != nullptr);
    require(dsdt->length == sizeof(acpi::TableHeader) + 16);
}

auto test_revision_one_fadt_uses_the_32_bit_pointer() -> void {
    // A revision 1 FADT is 116 bytes: it ends before x_dsdt, and the bytes
    // after it are not the table's.
    Machine machine;
    add_dsdt(machine, 16, 0x5a);
    add_fadt(machine, 116, u32(machine.unlisted_phys(0)), 0);
    machine.finish();
    auto const *dsdt = dsdt_of(machine);
    require(dsdt != nullptr);
    require(dsdt->length == sizeof(acpi::TableHeader) + 16);
}

auto test_no_usable_dsdt() -> void {
    // No FADT.
    Machine no_fadt;
    add_madt(no_fadt);
    no_fadt.finish();
    require(dsdt_of(no_fadt) == nullptr);

    // A FADT that points nowhere.
    Machine zero;
    add_fadt(zero, sizeof(acpi::Fadt), 0, 0);
    zero.finish();
    require(dsdt_of(zero) == nullptr);

    // A FADT whose declared length ends before the 32 bit pointer. The pointer
    // is in memory, past the table, and it is not the table's to read.
    Machine short_fadt;
    add_dsdt(short_fadt, 16, 0x5a);
    add_fadt(short_fadt, sizeof(acpi::Fadt), u32(short_fadt.unlisted_phys(0)), 0);
    reinterpret_cast<acpi::TableHeader *>(short_fadt.table_bytes(0))->length = 40;
    short_fadt.finish();
    require(dsdt_of(short_fadt) == nullptr);

    // The pointer leads to a table that is not a DSDT.
    Machine wrong_table;
    auto *ssdt = wrong_table.add_unlisted_table<acpi::TableHeader>(
        acpi::Signature::from_chars("SSDT"), sizeof(acpi::TableHeader) + 16);
    require(ssdt != nullptr);
    add_fadt(wrong_table, sizeof(acpi::Fadt), u32(wrong_table.unlisted_phys(0)), 0);
    wrong_table.finish();
    require(dsdt_of(wrong_table) == nullptr);

    // A DSDT whose checksum does not hold.
    Machine bad_checksum;
    add_dsdt(bad_checksum, 16, 0x5a);
    add_fadt(bad_checksum, sizeof(acpi::Fadt), u32(bad_checksum.unlisted_phys(0)), 0);
    bad_checksum.finish();
    bad_checksum.unlisted_bytes(0)[9] = u8(bad_checksum.unlisted_bytes(0)[9] + 1);
    require(dsdt_of(bad_checksum) == nullptr);

    // A pointer the mapper cannot follow.
    Machine unmapped;
    add_dsdt(unmapped, 16, 0x5a);
    add_fadt(unmapped, sizeof(acpi::Fadt), 0x7000'0000, 0);
    unmapped.finish();
    require(dsdt_of(unmapped) == nullptr);
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
    test_dsdt_is_found_through_the_fadt();
    test_x_dsdt_wins_over_dsdt();
    test_revision_one_fadt_uses_the_32_bit_pointer();
    test_no_usable_dsdt();
    return 0;
}
