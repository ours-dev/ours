/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The route service's logic against real directories: a synthesised machine's
/// tables, laid out by acpi::export_tables(), read back by TableDirectory, and
/// the two answers asked of them.

#include <ours/acpi_tables.hpp>

#include <vector>

#include <cstring>

#include "kernel/lib/acpi/test/acpi_test_blob.hpp"
#include <acpi/structures/madt.hpp>
#include <acpi/structures/mcfg.hpp>
#include <acpi/table_export.hpp>
#include <acpi_route/routes.hpp>
#include <acpi_route/table_directory.hpp>
#include <sys/mman.h>

namespace {

using acpi::test::Machine;
using acpi::test::require;
using acpi_route::InterruptRoute;
using acpi_route::Polarity;
using acpi_route::TableDirectory;
using acpi_route::Trigger;
using ours::Status;

/// One interrupt source override of the MADT.
struct Override {
    u8 bus;
    u8 source;
    u32 global_irq;
    u16 flags;
};

/// MPS INTI flags: polarity in bits 0-1, trigger mode in bits 2-3, each 0
/// "conforms", 1 high or edge, 3 low or level, 2 reserved.
constexpr auto CONFORMS = u16{0b0000};
constexpr auto EDGE_HIGH = u16{0b0101};
constexpr auto LEVEL_LOW = u16{0b1111};
constexpr auto RESERVED_POLARITY = u16{0b0010};
constexpr auto RESERVED_TRIGGER = u16{0b1000};

/// A MADT with an IO-APIC and `overrides`, then `tail` bytes of whatever the
/// caller makes of them (a malformed entry, say).
auto add_madt(Machine &machine, std::vector<Override> const &overrides, usize const tail = 0)
    -> u8 * {
    auto const length = sizeof(acpi::madt::Table) + sizeof(acpi::madt::IoApicEntry) +
                        overrides.size() * sizeof(acpi::madt::InterruptSourceOverrideEntry) + tail;
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

    for (auto const &spec: overrides) {
        auto *entry = reinterpret_cast<acpi::madt::InterruptSourceOverrideEntry *>(cursor);
        entry->header.type = u8(acpi::madt::Type::InterruptSourceOverride);
        entry->header.length = sizeof(acpi::madt::InterruptSourceOverrideEntry);
        entry->bus = spec.bus;
        entry->source = spec.source;
        entry->global_irq = spec.global_irq;
        entry->flags = spec.flags;
        cursor += sizeof(acpi::madt::InterruptSourceOverrideEntry);
    }
    return cursor; // the first of the `tail` bytes
}

/// An MCFG of `allocations`, then `partial` bytes of a half-written one.
auto add_mcfg(Machine &machine, std::vector<acpi::mcfg::Allocation> const &allocations,
              usize const partial = 0) -> void {
    auto const length =
        sizeof(acpi::mcfg::Table) + allocations.size() * sizeof(acpi::mcfg::Allocation) + partial;
    auto *mcfg = machine.add_table<acpi::mcfg::Table>(acpi::mcfg::Table::SIGNATURE, length);
    auto *cursor = reinterpret_cast<u8 *>(mcfg) + sizeof(acpi::mcfg::Table);
    for (auto const &allocation: allocations) {
        std::memcpy(cursor, &allocation, sizeof(allocation));
        cursor += sizeof(allocation);
    }
    std::memset(cursor, 0xff, partial);
}

/// The directory the kernel would hand user space for `machine`.
auto directory_bytes(Machine &machine) -> std::vector<u8> {
    machine.finish();
    auto tables = acpi::TableSet::from_rsdp(machine.mapper(), machine.rsdp_phys());
    require(bool(tables));
    auto const layout = acpi::plan_export(*tables);
    std::vector<u8> bytes(layout.size, 0);
    auto const used = acpi::export_tables(*tables, {bytes.data(), bytes.size()});
    require(bool(used));
    require(used.unwrap() == layout.size);
    return bytes;
}

auto read(std::vector<u8> const &bytes) -> ustl::Result<TableDirectory, Status> {
    return TableDirectory::from_bytes({bytes.data(), bytes.size()});
}

/// The directory of `machine`, which must be valid.
auto directory_of(Machine &machine, std::vector<u8> &storage) -> TableDirectory {
    storage = directory_bytes(machine);
    auto directory = read(storage);
    require(directory.is_ok());
    return directory.unwrap();
}

/// A machine that looks like QEMU's q35: ISA 0 on GSI 2, ISA 9 on GSI 9 level
/// triggered and active low.
auto qemu_like_machine(Machine &machine) -> void {
    add_madt(machine, {{0, 0, 2, CONFORMS}, {0, 9, 9, LEVEL_LOW}});
}

auto route_of(std::vector<u8> const &bytes, u32 const isa) -> ustl::Result<InterruptRoute, Status> {
    auto directory = read(bytes);
    require(directory.is_ok());
    return acpi_route::isa_irq_to_gsi(directory.unwrap(), isa);
}

auto expect_route(std::vector<u8> const &bytes, u32 const isa, u32 const gsi, Trigger const trigger,
                  Polarity const polarity) -> void {
    auto const route = route_of(bytes, isa);
    require(route.is_ok());
    require(route.unwrap().gsi == gsi);
    require(route.unwrap().trigger == trigger);
    require(route.unwrap().polarity == polarity);
}

auto expect_error(ustl::Result<InterruptRoute, Status> const &route, Status const status) -> void {
    require(route.is_err());
    require(route.unwrap_err() == status);
}

// ---------------------------------------------------------------- directory

auto test_a_directory_reads_back() -> void {
    // A DSDT the FADT points to and the root table does not list, then the
    // FADT, a MADT and an MCFG: the first table is the DSDT.
    Machine machine;
    auto *dsdt = machine.add_unlisted_table<acpi::TableHeader>(acpi::Signature::from_chars("DSDT"),
                                                               sizeof(acpi::TableHeader) + 21);
    require(dsdt != nullptr);
    auto *fadt = machine.add_table<acpi::Fadt>(acpi::Fadt::SIGNATURE, sizeof(acpi::Fadt));
    fadt->dsdt = u32(machine.unlisted_phys(0));
    qemu_like_machine(machine);
    add_mcfg(machine, {{0xe000'0000, 0, 0, 255, 0}});

    std::vector<u8> storage;
    auto const directory = directory_of(machine, storage);
    require(directory.count() == 4);
    require(directory.find(acpi::Signature::from_chars("DSDT")).size() ==
            sizeof(acpi::TableHeader) + 21);
    require(directory.find<acpi::Fadt>() != nullptr);
    require(directory.find<acpi::madt::Table>() != nullptr);
    require(directory.find<acpi::mcfg::Table>() != nullptr);
    require(directory.find(acpi::Signature::from_chars("SSDT")).empty());
}

/// Patches a copy of `good` with `patch`, and expects InvArgs from reading it.
template <typename Patch>
auto expect_rejected(std::vector<u8> const &good, Patch &&patch) -> void {
    auto bytes = good;
    patch(bytes);
    auto const directory = read(bytes);
    require(directory.is_err());
    require(directory.unwrap_err() == Status::InvArgs);
}

/// Where the field of entry `index` is in a directory: a 16 byte header, then
/// 16 bytes to an entry (signature, offset, length, reserved).
constexpr auto entry_field(usize const index, usize const field) -> usize {
    return sizeof(ours::AcpiDirectory) + index * sizeof(ours::AcpiDirectoryEntry) + field * 4;
}

auto put32(std::vector<u8> &bytes, usize const at, u32 const value) -> void {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}

auto get32(std::vector<u8> const &bytes, usize const at) -> u32 {
    u32 value;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

auto test_a_bad_directory_is_refused() -> void {
    Machine machine;
    qemu_like_machine(machine);
    add_mcfg(machine, {{0xe000'0000, 0, 0, 255, 0}});
    auto const good = directory_bytes(machine);
    require(read(good).is_ok());

    // The header.
    expect_rejected(good, [](auto &b) { b.resize(8); });
    expect_rejected(good, [](auto &b) { put32(b, 0, 0); });
    expect_rejected(good, [](auto &b) { put32(b, 4, 2); });
    expect_rejected(good, [](auto &b) { put32(b, 8, 0xffff'ffff); }); // a count no size can hold
    expect_rejected(good, [](auto &b) { put32(b, 8, 100); }); // the entries would not fit
    expect_rejected(good, [](auto &b) { put32(b, 12, u32(b.size()) + 1); });
    expect_rejected(good, [](auto &b) { b.pop_back(); }); // fewer bytes than the size says

    // An entry.
    expect_rejected(good, [](auto &b) { put32(b, entry_field(0, 3), 1); });
    expect_rejected(good,
                    [](auto &b) { put32(b, entry_field(0, 1), get32(b, entry_field(0, 1)) + 1); });
    expect_rejected(good, [](auto &b) { put32(b, entry_field(0, 1), 0); });
    expect_rejected(good, [](auto &b) { put32(b, entry_field(0, 2), 10); });
    expect_rejected(good, [](auto &b) { put32(b, entry_field(0, 2), 0x1000'0000); });
    expect_rejected(good, [](auto &b) { put32(b, entry_field(0, 0), 0x4141'4141); });
    expect_rejected(good,
                    [](auto &b) { put32(b, entry_field(0, 2), get32(b, entry_field(0, 2)) - 8); });
    // Two entries for one table, so that its header and checksum agree with
    // both and only the overlap is wrong.
    expect_rejected(good, [](auto &b) {
        for (auto const field: {usize{0}, usize{1}, usize{2}}) {
            put32(b, entry_field(1, field), get32(b, entry_field(0, field)));
        }
    });

    // A table whose bytes do not add up to zero.
    expect_rejected(good, [](auto &b) { b[get32(b, entry_field(1, 1)) + 40] ^= 1; });
}

/// A directory of `count` tables made by hand, each a "TEST" table of `length`
/// bytes with a good checksum, the first at the first aligned offset after the
/// entries and each next one `stride` bytes after the one before. For the
/// limits a real machine never gets near, and for the checks that a directory
/// made of real tables cannot isolate: a stride that is not a multiple of 8
/// puts a table where only the alignment check refuses it.
auto hand_made_directory(u32 const count, usize const stride = 40, usize const length = 40)
    -> std::vector<u8> {
    auto const entries_end =
        sizeof(ours::AcpiDirectory) + usize{count} * sizeof(ours::AcpiDirectoryEntry);
    auto const tables_begin = (entries_end + 7) & ~usize{7};
    // Room for a table of the full header's size at the last offset, whatever
    // the length: make_table_header() writes all 36 bytes of it.
    std::vector<u8> bytes(tables_begin + usize{count} * stride + 40, 0);

    auto const directory = ours::AcpiDirectory{
        .magic = ours::ACPI_DIRECTORY_MAGIC,
        .version = ours::ACPI_DIRECTORY_VERSION,
        .count = count,
        .size = u32(tables_begin + usize{count} * stride),
    };
    std::memcpy(bytes.data(), &directory, sizeof(directory));
    auto const sign = acpi::Signature::from_chars("TEST");
    for (auto index = u32{0}; index < count; ++index) {
        auto const offset = tables_begin + usize{index} * stride;
        auto const entry = ours::AcpiDirectoryEntry{
            .signature = sign.value,
            .offset = u32(offset),
            .length = u32(length),
            .reserved = 0,
        };
        std::memcpy(bytes.data() + sizeof(directory) + usize{index} * sizeof(entry), &entry,
                    sizeof(entry));
        acpi::test::make_table_header(bytes.data() + offset, sign, u32(length));
    }
    return bytes;
}

/// Whether from_bytes() accepts a copy of `bytes` that ends at the last byte of
/// a mapped page, the page after it being unmapped: a read past the end is a
/// crash, where in an ordinary buffer it would read whatever follows and
/// usually go unnoticed. The checks that only keep the reader inside its bytes
/// can be seen failing no other way without a sanitizer.
auto accepted_at_the_edge(std::vector<u8> const &bytes) -> bool {
    constexpr auto PAGE = usize{4096};
    auto *const mapping =
        mmap(nullptr, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(mapping != MAP_FAILED);
    auto *const page = static_cast<u8 *>(mapping);
    require(mprotect(page + PAGE, PAGE, PROT_NONE) == 0);
    auto *const data = page + PAGE - bytes.size();
    std::memcpy(data, bytes.data(), bytes.size());
    auto const accepted = TableDirectory::from_bytes({data, bytes.size()}).is_ok();
    munmap(mapping, 2 * PAGE);
    return accepted;
}

/// Re-seals the checksum of the table `length` bytes long at `offset`: the sum
/// of the bytes is zero again after a test changed the table.
auto reseal(std::vector<u8> &bytes, usize const offset, usize const length) -> void {
    bytes[offset + 9] = 0;
    auto sum = u8{0};
    for (auto index = usize{0}; index < length; ++index) {
        sum = u8(sum + bytes[offset + index]);
    }
    bytes[offset + 9] = u8(0u - sum);
}

auto test_a_directory_may_list_a_great_many_tables() -> void {
    // No count of tables is refused for being large: the walk is bounded by the
    // directory's size, which is checked against the bytes there are.
    auto const many = hand_made_directory(2000);
    auto const directory = read(many);
    require(directory.is_ok());
    require(directory.unwrap().count() == 2000);
}

/// The checks of from_bytes() that a directory made of real tables cannot
/// isolate: each case breaks one thing and leaves every other check satisfied,
/// the table's own header and checksum included, so that removing the one check
/// that is meant to refuse it makes the case pass.
auto test_each_check_of_the_reader_stands_alone() -> void {
    auto const good = hand_made_directory(3);
    require(read(good).is_ok());

    // A table that reaches past the directory's size, inside the bytes there
    // are (the buffer is longer than the size says), with a header and a sum
    // that agree with the entry.
    {
        auto bytes = good;
        auto const last = entry_field(2, 0);
        auto const offset = get32(bytes, last + 4);
        auto const size = get32(bytes, 12);
        auto const length = size - offset + 24;
        bytes.resize(size + 64, 0);
        put32(bytes, last + 8, length);
        put32(bytes, offset + 4, length);
        reseal(bytes, offset, length);
        require(read(bytes).is_err());
    }
    // A header whose length is not the entry's, the sum over the entry's length
    // good.
    {
        auto bytes = good;
        auto const offset = get32(bytes, entry_field(1, 0) + 4);
        put32(bytes, offset + 4, get32(bytes, entry_field(1, 0) + 8) + 8);
        reseal(bytes, offset, get32(bytes, entry_field(1, 0) + 8));
        require(read(bytes).is_err());
    }
    // A table shorter than a table header.
    {
        auto bytes = good;
        auto const first = entry_field(0, 0);
        auto const offset = get32(bytes, first + 4);
        put32(bytes, first + 8, 20);
        put32(bytes, offset + 4, 20);
        reseal(bytes, offset, 20);
        require(read(bytes).is_err());
    }
    // Entries that do not fit in the directory's size: a directory of 20 bytes
    // that claims one entry, and one that claims a count no size can hold. The
    // check only keeps the reader from reading the entries past the bytes, so it
    // is seen failing at the edge of a mapped page.
    {
        auto bytes = good;
        bytes.resize(20);
        put32(bytes, 8, 1);
        put32(bytes, 12, 20);
        require(!accepted_at_the_edge(bytes));
        put32(bytes, 8, 0xffff'ffff);
        require(!accepted_at_the_edge(bytes));
    }
    // A table at an offset that is not a multiple of 8, after a gap, so that
    // the ordering is satisfied: a stride of 44 puts the second table 4 bytes
    // past a multiple of 8.
    {
        auto const unaligned = hand_made_directory(2, 44, 36);
        require(read(unaligned).is_err());
        auto const aligned = hand_made_directory(2, 48, 36);
        require(read(aligned).is_ok());
    }
}

// ------------------------------------------------------------------- ISA IRQ

auto test_an_isa_line_with_no_override_is_its_own_gsi() -> void {
    Machine machine;
    add_madt(machine, {});
    auto const bytes = directory_bytes(machine);
    expect_route(bytes, 0, 0, Trigger::Edge, Polarity::ActiveHigh);
    expect_route(bytes, 1, 1, Trigger::Edge, Polarity::ActiveHigh);
    expect_route(bytes, 15, 15, Trigger::Edge, Polarity::ActiveHigh);
}

auto test_overrides_name_the_gsi_and_the_signalling() -> void {
    Machine machine;
    qemu_like_machine(machine);
    auto const bytes = directory_bytes(machine);
    // The timer is on GSI 2 and conforms to the bus.
    expect_route(bytes, 0, 2, Trigger::Edge, Polarity::ActiveHigh);
    // A line the firmware wired level triggered and active low.
    expect_route(bytes, 9, 9, Trigger::Level, Polarity::ActiveLow);
    // The keyboard has none: it is GSI 1.
    expect_route(bytes, 1, 1, Trigger::Edge, Polarity::ActiveHigh);
}

auto test_the_flags_are_two_bit_fields() -> void {
    Machine machine;
    add_madt(machine, {
                          {0, 3, 30, EDGE_HIGH},
                          {0, 4, 31, 0b0111}, // edge, polarity 3: low
                          {0, 5, 32, 0b1101}, // level, polarity 1: high
                      });
    auto const bytes = directory_bytes(machine);
    expect_route(bytes, 3, 30, Trigger::Edge, Polarity::ActiveHigh);
    expect_route(bytes, 4, 31, Trigger::Edge, Polarity::ActiveLow);
    expect_route(bytes, 5, 32, Trigger::Level, Polarity::ActiveHigh);
}

auto test_the_last_override_of_a_line_wins() -> void {
    // The kernel keeps one override for each ISA number, and a later entry
    // replaces an earlier one, so the line it programs is the later entry's.
    Machine machine;
    add_madt(machine, {{0, 4, 10, CONFORMS}, {0, 4, 11, LEVEL_LOW}});
    expect_route(directory_bytes(machine), 4, 11, Trigger::Level, Polarity::ActiveLow);
}

auto test_the_bus_field_is_not_read() -> void {
    // The MADT fixes the bus of an override at 0, and the kernel does not read
    // it: an entry on another bus is honoured, because it is the pin the
    // kernel programmed.
    Machine machine;
    add_madt(machine, {{1, 3, 20, LEVEL_LOW}});
    expect_route(directory_bytes(machine), 3, 20, Trigger::Level, Polarity::ActiveLow);
}

auto test_no_line_for_the_cascade_or_past_the_isa_lines() -> void {
    Machine machine;
    // Even an override of the cascade does not make it a line.
    add_madt(machine, {{0, 2, 22, CONFORMS}});
    auto const bytes = directory_bytes(machine);
    expect_error(route_of(bytes, 2), Status::NotFound);
    expect_error(route_of(bytes, 16), Status::NotFound);
    expect_error(route_of(bytes, 0x100), Status::NotFound);
    expect_error(route_of(bytes, 0xffff'ffff), Status::NotFound);
}

auto test_a_flag_that_cannot_be_honoured_is_refused() -> void {
    Machine reserved_polarity;
    add_madt(reserved_polarity, {{0, 6, 6, RESERVED_POLARITY}});
    expect_error(route_of(directory_bytes(reserved_polarity), 6), Status::InvArgs);

    Machine reserved_trigger;
    add_madt(reserved_trigger, {{0, 6, 6, RESERVED_TRIGGER}});
    expect_error(route_of(directory_bytes(reserved_trigger), 6), Status::InvArgs);
}

auto test_no_madt_is_not_found() -> void {
    Machine machine;
    add_mcfg(machine, {{0xe000'0000, 0, 0, 255, 0}});
    expect_error(route_of(directory_bytes(machine), 1), Status::NotFound);
}

auto test_a_madt_that_cannot_be_read_to_its_end_ends_the_walk_where_the_kernel_does() -> void {
    // An override of ISA 1, then an entry whose length is below the minimum,
    // then an override of ISA 7 that the walk never reaches. The kernel's walk
    // stops at the bad entry and looks no further, so it programs ISA 7 on its
    // own pin, and so does this.
    Machine machine;
    constexpr auto OVERRIDE = sizeof(acpi::madt::InterruptSourceOverrideEntry);
    auto *tail = add_madt(machine, {{0, 1, 5, CONFORMS}}, 2 + OVERRIDE);
    tail[0] = u8(acpi::madt::Type::LocalApic);
    tail[1] = 0;
    acpi::madt::InterruptSourceOverrideEntry unreached{};
    unreached.header.type = u8(acpi::madt::Type::InterruptSourceOverride);
    unreached.header.length = u8(OVERRIDE);
    unreached.source = 7;
    unreached.global_irq = 30;
    std::memcpy(tail + 2, &unreached, OVERRIDE);
    auto const bytes = directory_bytes(machine);
    expect_route(bytes, 1, 5, Trigger::Edge, Polarity::ActiveHigh);
    expect_route(bytes, 7, 7, Trigger::Edge, Polarity::ActiveHigh);
}

// --------------------------------------------------------------------- ECAM

auto ecam_of(std::vector<u8> const &bytes, u16 const segment)
    -> ustl::Result<acpi_route::EcamWindow, Status> {
    auto directory = read(bytes);
    require(directory.is_ok());
    return acpi_route::ecam_window(directory.unwrap(), segment);
}

auto test_the_ecam_window_of_a_segment() -> void {
    Machine machine;
    add_mcfg(machine, {
                          {0xe000'0000, 0, 0, 255, 0},
                          // Buses 16 to 31 of segment 1: the base is where bus 0
                          // would be, so the window starts 16 MiB in.
                          {0x4'0000'0000, 1, 16, 31, 0},
                      });
    auto const bytes = directory_bytes(machine);

    auto const first = ecam_of(bytes, 0);
    require(first.is_ok());
    require(first.unwrap().address == 0xe000'0000);
    require(first.unwrap().size == u64{256} << 20);
    require(first.unwrap().segment == 0);
    require(first.unwrap().first_bus == 0);
    require(first.unwrap().last_bus == 255);

    auto const second = ecam_of(bytes, 1);
    require(second.is_ok());
    require(second.unwrap().address == 0x4'0000'0000 + (u64{16} << 20));
    require(second.unwrap().size == u64{16} << 20);
    require(second.unwrap().first_bus == 16);
    require(second.unwrap().last_bus == 31);

    require(ecam_of(bytes, 2).is_err());
    require(ecam_of(bytes, 2).unwrap_err() == Status::NotFound);
}

auto test_a_half_written_allocation_is_not_one() -> void {
    Machine machine;
    // Ten bytes: a base and a segment of 0xffff, then nothing.
    add_mcfg(machine, {{0xe000'0000, 0, 0, 255, 0}}, 10);
    auto const bytes = directory_bytes(machine);
    require(ecam_of(bytes, 0).is_ok());
    require(ecam_of(bytes, 0xffff).unwrap_err() == Status::NotFound);
}

auto test_a_bad_allocation_is_refused() -> void {
    Machine backwards;
    add_mcfg(backwards, {{0xe000'0000, 0, 9, 8, 0}});
    require(ecam_of(directory_bytes(backwards), 0).unwrap_err() == Status::InvArgs);

    // A window that would run past the end of the address space.
    Machine overflowing;
    add_mcfg(overflowing, {{0xffff'ffff'fff0'0000, 0, 0, 1, 0}});
    require(ecam_of(directory_bytes(overflowing), 0).unwrap_err() == Status::InvArgs);

    // A base that, with the first bus added, wraps.
    Machine wrapping;
    add_mcfg(wrapping, {{0xffff'ffff'ffff'0000, 0, 255, 255, 0}});
    require(ecam_of(directory_bytes(wrapping), 0).unwrap_err() == Status::InvArgs);
}

auto test_no_mcfg_is_not_found() -> void {
    Machine machine;
    add_madt(machine, {});
    require(ecam_of(directory_bytes(machine), 0).unwrap_err() == Status::NotFound);
}

} // namespace

auto main() -> int {
    test_a_directory_reads_back();
    test_a_bad_directory_is_refused();
    test_a_directory_may_list_a_great_many_tables();
    test_each_check_of_the_reader_stands_alone();
    test_an_isa_line_with_no_override_is_its_own_gsi();
    test_overrides_name_the_gsi_and_the_signalling();
    test_the_flags_are_two_bit_fields();
    test_the_last_override_of_a_line_wins();
    test_the_bus_field_is_not_read();
    test_no_line_for_the_cascade_or_past_the_isa_lines();
    test_a_flag_that_cannot_be_honoured_is_refused();
    test_no_madt_is_not_found();
    test_a_madt_that_cannot_be_read_to_its_end_ends_the_walk_where_the_kernel_does();
    test_the_ecam_window_of_a_segment();
    test_a_half_written_allocation_is_not_one();
    test_a_bad_allocation_is_refused();
    test_no_mcfg_is_not_found();
    return 0;
}
