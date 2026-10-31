/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// export_tables(): the layout user space is handed (<ours/acpi_tables.hpp>)
/// and what it leaves out.

#include <acpi/table_export.hpp>

#include <functional>
#include <vector>

#include <cstring>

#include "acpi_test_blob.hpp"
#include <ours/acpi_tables.hpp>

namespace {

using acpi::test::Machine;
using acpi::test::require;

constexpr auto DSDT = acpi::Signature::from_chars("DSDT");
constexpr auto APIC = acpi::Signature::from_chars("APIC");
constexpr auto SSDT = acpi::Signature::from_chars("SSDT");
constexpr auto FACP = acpi::Fadt::SIGNATURE;

/// A table of `body` bytes after its header, every one `fill`, in the root
/// table's list.
auto add_listed(Machine &machine, acpi::Signature const sign, usize const body, u8 const fill)
    -> void {
    auto *table = machine.add_table<acpi::TableHeader>(sign, sizeof(acpi::TableHeader) + body);
    std::memset(reinterpret_cast<u8 *>(table) + sizeof(acpi::TableHeader), fill, body);
}

/// The machine most tests use: a DSDT the FADT points to and the root table
/// does not list, then the FADT, an APIC table and two SSDTs in the root
/// table, every length a different distance from a multiple of 8.
///
///   listed 0 FACP 148 bytes, 1 APIC 46, 2 SSDT 69, 3 SSDT 43; unlisted 0 DSDT 57
auto build(Machine &machine) -> void {
    auto *dsdt =
        machine.add_unlisted_table<acpi::TableHeader>(DSDT, sizeof(acpi::TableHeader) + 21);
    std::memset(reinterpret_cast<u8 *>(dsdt) + sizeof(acpi::TableHeader), 0xd5, 21);

    auto *fadt = machine.add_table<acpi::Fadt>(FACP, sizeof(acpi::Fadt));
    fadt->dsdt = u32(machine.unlisted_phys(0));

    add_listed(machine, APIC, 10, 0xa1);
    add_listed(machine, SSDT, 33, 0x51);
    add_listed(machine, SSDT, 7, 0x52);
    machine.finish();
}

struct Exported {
    std::vector<u8> bytes;
    ours::AcpiDirectory directory{};
    std::vector<ours::AcpiDirectoryEntry> entries;
};

/// Exports `machine`'s tables into a buffer 64 bytes longer than needed, the
/// extra filled with 0xaa so that a write past the plan shows.
auto export_from(Machine &machine) -> Exported {
    auto tables = acpi::TableSet::from_rsdp(machine.mapper(), machine.rsdp_phys());
    require(bool(tables));

    auto const layout = acpi::plan_export(*tables);
    Exported exported;
    exported.bytes.assign(layout.size + 64, 0xaa);
    auto const used = acpi::export_tables(*tables, {exported.bytes.data(), exported.bytes.size()});
    require(bool(used));
    require(used.unwrap() == layout.size);

    std::memcpy(&exported.directory, exported.bytes.data(), sizeof(exported.directory));
    exported.entries.resize(exported.directory.count);
    std::memcpy(exported.entries.data(), exported.bytes.data() + sizeof(exported.directory),
                exported.entries.size() * sizeof(ours::AcpiDirectoryEntry));
    return exported;
}

auto test_layout() -> void {
    Machine machine;
    build(machine);
    auto const exported = export_from(machine);

    // 96 bytes of directory (16 + 5 * 16 rounded to 8), then the tables each
    // rounded to 8: 57 -> 64, 148 -> 152, 46 -> 48, 69 -> 72, 43 -> 48. Written
    // out here, so that a change to the format cannot pass by changing both
    // sides alike.
    constexpr auto SIZE = usize{96 + 64 + 152 + 48 + 72 + 48};
    require(exported.directory.magic == 0x4950'4341);
    require(exported.directory.version == 1);
    require(exported.directory.count == 5);
    require(exported.directory.size == SIZE);

    // The DSDT first, then the root table's order.
    acpi::Signature const order[] = {DSDT, FACP, APIC, SSDT, SSDT};
    constexpr usize LENGTH[] = {57, 148, 46, 69, 43};
    constexpr usize OFFSET[] = {96, 160, 312, 360, 432};
    for (auto i = 0; i < 5; ++i) {
        auto const &entry = exported.entries[usize(i)];
        require(entry.signature == order[i].value);
        require(entry.length == LENGTH[i]);
        require(entry.offset == OFFSET[i]);
        require(entry.reserved == 0);
        require(entry.offset % ours::ACPI_TABLE_ALIGNMENT == 0);
    }

    // Each table's bytes are the firmware's, exactly.
    require(std::memcmp(exported.bytes.data() + OFFSET[0], machine.unlisted_bytes(0), LENGTH[0]) ==
            0);
    for (auto i = 1; i < 5; ++i) {
        require(std::memcmp(exported.bytes.data() + OFFSET[i], machine.table_bytes(usize(i - 1)),
                            LENGTH[i]) == 0);
    }

    // Every byte that is not the directory or a table is zero, and every byte
    // past the size is what the buffer held before.
    std::vector<bool> covered(exported.bytes.size(), false);
    for (auto i = usize(0); i < 96; ++i) {
        covered[i] = true;
    }
    for (auto i = 0; i < 5; ++i) {
        for (auto b = usize(0); b < LENGTH[i]; ++b) {
            covered[OFFSET[i] + b] = true;
        }
    }
    for (auto i = usize(0); i < exported.bytes.size(); ++i) {
        if (i >= SIZE) {
            require(exported.bytes[i] == 0xaa);
        } else if (!covered[i]) {
            require(exported.bytes[i] == 0);
        }
    }
}

auto test_buffer_too_small() -> void {
    Machine machine;
    build(machine);
    auto tables = acpi::TableSet::from_rsdp(machine.mapper(), machine.rsdp_phys());
    require(bool(tables));

    auto const layout = acpi::plan_export(*tables);
    std::vector<u8> bytes(layout.size - 1, 0xaa);
    auto const used = acpi::export_tables(*tables, {bytes.data(), bytes.size()});
    require(!used);
    require(used.error() == acpi::Status::BufferTooSmall);
    for (auto const byte: bytes) {
        require(byte == 0xaa);
    }
}

auto test_the_plan_counts_what_is_left_out() -> void {
    Machine machine;
    build(machine);
    {
        auto tables = acpi::TableSet::from_rsdp(machine.mapper(), machine.rsdp_phys());
        require(bool(tables));
        auto const layout = acpi::plan_export(*tables);
        require(layout.count == 5);
        require(layout.skipped == 0);
        require(layout.has_dsdt);
    }
    // A bad checksum in the root table's APIC and the DSDT: one skipped, and no
    // DSDT.
    machine.table_bytes(1)[9] = u8(machine.table_bytes(1)[9] + 1);
    machine.unlisted_bytes(0)[9] = u8(machine.unlisted_bytes(0)[9] + 1);
    auto tables = acpi::TableSet::from_rsdp(machine.mapper(), machine.rsdp_phys());
    require(bool(tables));
    auto const layout = acpi::plan_export(*tables);
    require(layout.count == 3);
    require(layout.skipped == 1);
    require(!layout.has_dsdt);
}

/// A mapper that, once armed, runs `change` just before it answers the
/// `after`-th call from then on: the firmware's tables changing between the
/// plan export_tables() makes and the pass that writes. It counts calls, so a
/// test can measure how many one plan takes and arm the change for the first
/// call after.
class ChangingMapper final: public acpi::PhysMapper {
  public:
    explicit ChangingMapper(acpi::test::HostMapper &real) : m_real(real) {}

    auto calls() const -> usize {
        return m_calls;
    }

    auto arm(usize const after, std::function<void()> change) -> void {
        m_fire_at = m_calls + after;
        m_change = std::move(change);
    }

    auto map(acpi::PhysAddr const addr, usize const size) -> ktl::Result<void const *> override {
        ++m_calls;
        if (m_change && m_calls == m_fire_at) {
            auto change = std::move(m_change);
            m_change = nullptr;
            change();
        }
        return m_real.map(addr, size);
    }

  private:
    acpi::test::HostMapper &m_real;
    std::function<void()> m_change;
    usize m_calls = 0;
    usize m_fire_at = 0;
};

/// How many calls to the mapper one plan makes.
auto calls_of_a_plan(acpi::TableSet &tables, ChangingMapper const &mapper) -> usize {
    auto const before = mapper.calls();
    auto const layout = acpi::plan_export(tables);
    require(layout.count != 0);
    return mapper.calls() - before;
}

auto test_tables_that_change_between_the_passes_are_not_a_directory() -> void {
    // A table whose checksum stops holding after the plan counted it. The write
    // pass leaves it out, so what it wrote is one table short of what the
    // directory would say: an error, and not a directory that claims five
    // tables and holds four.
    Machine machine;
    build(machine);
    ChangingMapper mapper(machine.mapper());
    auto tables = acpi::TableSet::from_rsdp(mapper, machine.rsdp_phys());
    require(bool(tables));

    auto const layout = acpi::plan_export(*tables);
    require(layout.count == 5);
    std::vector<u8> bytes(layout.size, 0xaa);
    // export_tables() plans first, as many calls as a plan takes; the change
    // lands on the first call after, which is the write pass.
    mapper.arm(calls_of_a_plan(*tables, mapper) + 1,
               [&] { machine.table_bytes(1)[9] = u8(machine.table_bytes(1)[9] + 1); });
    auto const used = acpi::export_tables(*tables, {bytes.data(), bytes.size()});
    require(!used);
    require(used.error() == acpi::Status::BadState);

    // A table that grows after the plan counted it, to a length the plan has no
    // room for: the same, and nothing is written past the buffer.
    Machine growing;
    build(growing);
    ChangingMapper growing_mapper(growing.mapper());
    auto growing_tables = acpi::TableSet::from_rsdp(growing_mapper, growing.rsdp_phys());
    require(bool(growing_tables));
    auto const growing_layout = acpi::plan_export(*growing_tables);
    std::vector<u8> growing_bytes(growing_layout.size + 16, 0xaa);
    growing_mapper.arm(calls_of_a_plan(*growing_tables, growing_mapper) + 1, [&] {
        // The SSDT at listed index 2 now claims to be 8 bytes longer than its
        // allocation holds; the sum no longer holds either, so it is left out.
        auto *header = reinterpret_cast<acpi::TableHeader *>(growing.table_bytes(2));
        header->length += 8;
    });
    auto const growing_used =
        acpi::export_tables(*growing_tables, {growing_bytes.data(), growing_bytes.size()});
    require(!growing_used);
    for (auto index = growing_layout.size; index < growing_bytes.size(); ++index) {
        require(growing_bytes[index] == 0xaa);
    }
}

auto test_bad_checksum_is_left_out() -> void {
    Machine machine;
    build(machine);
    machine.table_bytes(1)[9] = u8(machine.table_bytes(1)[9] + 1); // the APIC table

    auto const exported = export_from(machine);
    require(exported.directory.count == 4);
    for (auto const &entry: exported.entries) {
        require(entry.signature != APIC.value);
    }
}

auto test_bad_dsdt_is_left_out() -> void {
    Machine machine;
    build(machine);
    machine.unlisted_bytes(0)[9] = u8(machine.unlisted_bytes(0)[9] + 1);

    auto const exported = export_from(machine);
    require(exported.directory.count == 4);
    require(exported.entries[0].signature == FACP.value);
    for (auto const &entry: exported.entries) {
        require(entry.signature != DSDT.value);
    }
}

auto test_a_dsdt_in_the_root_table_is_ignored() -> void {
    // A DSDT the root table lists as well: the FADT's is the one exported, and
    // only once.
    Machine machine;
    auto *dsdt =
        machine.add_unlisted_table<acpi::TableHeader>(DSDT, sizeof(acpi::TableHeader) + 21);
    std::memset(reinterpret_cast<u8 *>(dsdt) + sizeof(acpi::TableHeader), 0xd5, 21);
    auto *fadt = machine.add_table<acpi::Fadt>(FACP, sizeof(acpi::Fadt));
    fadt->dsdt = u32(machine.unlisted_phys(0));
    add_listed(machine, DSDT, 5, 0x77);
    machine.finish();

    auto const exported = export_from(machine);
    require(exported.directory.count == 2);
    require(exported.entries[0].signature == DSDT.value);
    require(exported.entries[0].length == sizeof(acpi::TableHeader) + 21);
    require(exported.bytes[exported.entries[0].offset + sizeof(acpi::TableHeader)] == 0xd5);
    require(exported.entries[1].signature == FACP.value);
}

auto test_no_dsdt() -> void {
    // Without a FADT there is no DSDT to find; the root table's tables are
    // still exported.
    Machine machine;
    add_listed(machine, APIC, 10, 0xa1);
    add_listed(machine, SSDT, 33, 0x51);
    machine.finish();

    auto const exported = export_from(machine);
    require(exported.directory.count == 2);
    require(exported.entries[0].signature == APIC.value);
    require(exported.entries[1].signature == SSDT.value);
}

} // namespace

auto main() -> int {
    test_layout();
    test_buffer_too_small();
    test_the_plan_counts_what_is_left_out();
    test_tables_that_change_between_the_passes_are_not_a_directory();
    test_bad_checksum_is_left_out();
    test_bad_dsdt_is_left_out();
    test_a_dsdt_in_the_root_table_is_ignored();
    test_no_dsdt();
    return 0;
}
