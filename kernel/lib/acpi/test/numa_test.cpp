/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// SRAT and SLIT decoding.

#include <acpi/numa.hpp>

#include <vector>

#include <cstdlib>

#include "acpi_test_blob.hpp"

namespace {

using acpi::test::Machine;
using acpi::test::require;

struct SratLayout {
    acpi::srat::Table *table;
    acpi::srat::MemoryAffinityEntry *memory;
    acpi::srat::ProcessorAffinityEntry *processor;
    acpi::srat::ProcessorX2ApicAffinityEntry *x2apic;
};

auto add_srat(Machine &machine) -> SratLayout {
    auto const length = sizeof(acpi::srat::Table) + sizeof(acpi::srat::MemoryAffinityEntry) +
                        sizeof(acpi::srat::ProcessorAffinityEntry) +
                        sizeof(acpi::srat::ProcessorX2ApicAffinityEntry);
    auto *srat = machine.add_table<acpi::srat::Table>(acpi::srat::Table::SIGNATURE, length);

    auto *cursor = reinterpret_cast<u8 *>(srat) + sizeof(acpi::srat::Table);

    auto *memory = reinterpret_cast<acpi::srat::MemoryAffinityEntry *>(cursor);
    memory->header.type = u8(acpi::srat::Type::MemoryAffinity);
    memory->header.length = sizeof(acpi::srat::MemoryAffinityEntry);
    memory->proximity_domain = 1;
    memory->base_address_low = 0x1000;
    memory->base_address_high = 0x2;
    memory->length_low = 0x3000;
    memory->length_high = 0x4;
    memory->flags = acpi::srat::Flag::Enabled;
    cursor += sizeof(acpi::srat::MemoryAffinityEntry);

    auto *processor = reinterpret_cast<acpi::srat::ProcessorAffinityEntry *>(cursor);
    processor->header.type = u8(acpi::srat::Type::ProcessorAffinity);
    processor->header.length = sizeof(acpi::srat::ProcessorAffinityEntry);
    processor->apic_id = 0x11;
    processor->proximity_domain_low = 3;
    processor->flags = acpi::srat::Flag::Enabled;
    cursor += sizeof(acpi::srat::ProcessorAffinityEntry);

    auto *x2apic = reinterpret_cast<acpi::srat::ProcessorX2ApicAffinityEntry *>(cursor);
    x2apic->header.type = u8(acpi::srat::Type::ProcessorX2ApicAffinity);
    x2apic->header.length = sizeof(acpi::srat::ProcessorX2ApicAffinityEntry);
    x2apic->x2apic_id = 0x1234;
    x2apic->proximity_domain = 5;
    x2apic->flags = acpi::srat::Flag::Enabled;

    return SratLayout{srat, memory, processor, x2apic};
}

auto test_memory_regions() -> void {
    Machine machine;
    auto layout = add_srat(machine);
    machine.finish();

    std::vector<acpi::numa::MemoryRegion> seen;
    auto status = acpi::numa::for_each_memory_region(
        *layout.table, [&](acpi::numa::MemoryRegion const &region) { seen.push_back(region); });
    require(status == acpi::Status::Ok);
    require(seen.size() == 1);
    require(seen[0].domain == 1);
    require(seen[0].base == 0x2'0000'1000ull);
    require(seen[0].size == 0x4'0000'3000ull);

    // A disabled region is skipped, not reported.
    layout.memory->flags = acpi::srat::Flag{0};
    seen.clear();
    status = acpi::numa::for_each_memory_region(
        *layout.table, [&](acpi::numa::MemoryRegion const &region) { seen.push_back(region); });
    require(status == acpi::Status::Ok);
    require(seen.empty());
}

auto test_cpu_bindings_skip_disabled() -> void {
    Machine machine;
    auto layout = add_srat(machine);
    machine.finish();

    std::vector<acpi::numa::CpuBinding> seen;
    auto collect = [&](acpi::numa::CpuBinding const &binding) { seen.push_back(binding); };

    auto status = acpi::numa::for_each_cpu_binding(*layout.table, collect);
    require(status == acpi::Status::Ok);
    require(seen.size() == 2);
    require(seen[0].apic_id == 0x11);
    require(seen[0].domain == 3);
    require(seen[1].apic_id == 0x1234);
    require(seen[1].domain == 5);

    // The regression this whole function exists for: a disabled processor is a
    // slot firmware described but did not populate.  It must be skipped.  The
    // old code returned InternalError, which failed NUMA bring-up entirely on
    // any machine that declared spare CPU capacity.
    layout.processor->flags = acpi::srat::Flag{0};
    seen.clear();
    status = acpi::numa::for_each_cpu_binding(*layout.table, collect);
    require(status == acpi::Status::Ok);
    require(seen.size() == 1);
    require(seen[0].apic_id == 0x1234);

    layout.x2apic->flags = acpi::srat::Flag{0};
    seen.clear();
    status = acpi::numa::for_each_cpu_binding(*layout.table, collect);
    require(status == acpi::Status::Ok);
    require(seen.empty());
}

auto test_distance_matrix() -> void {
    Machine machine;
    constexpr u64 COUNT = 3;
    auto const length = sizeof(acpi::slit::Table) + COUNT * COUNT;
    auto *slit = machine.add_table<acpi::slit::Table>(acpi::slit::Table::SIGNATURE, length);
    slit->locality_count = COUNT;
    for (u64 i = 0; i < COUNT; ++i) {
        for (u64 j = 0; j < COUNT; ++j) {
            slit->entry[i * COUNT + j] = u8(i == j ? 10 : 20 + i * COUNT + j);
        }
    }
    machine.finish();

    auto matrix = acpi::numa::DistanceMatrix::from_table(*slit);
    require(bool(matrix));
    require(matrix->count() == 3);
    require(matrix->distance(0, 0) == 10);
    require(matrix->distance(1, 1) == 10);
    require(matrix->distance(0, 2) == 22);
    require(matrix->distance(2, 1) == 27);
    // Out of range indices are clamped rather than read past the payload.
    require(matrix->distance(3, 0) == 0);
    require(matrix->distance(0, 3) == 0);
}

auto test_distance_matrix_rejects_lying_count() -> void {
    Machine machine;
    // Room for a 2x2 matrix, but the table claims 64 localities.  Trusting that
    // count would index 4096 bytes into a 48 byte payload.
    auto const length = sizeof(acpi::slit::Table) + 4;
    auto *slit = machine.add_table<acpi::slit::Table>(acpi::slit::Table::SIGNATURE, length);
    slit->locality_count = 64;
    machine.finish();

    require(!acpi::numa::DistanceMatrix::from_table(*slit));

    // A count that would overflow a u32 is refused before it is narrowed.
    slit->locality_count = 0x1'0000'0000ull;
    require(!acpi::numa::DistanceMatrix::from_table(*slit));

    // Exactly filling the payload is fine.
    slit->locality_count = 2;
    auto matrix = acpi::numa::DistanceMatrix::from_table(*slit);
    require(bool(matrix));
    require(matrix->count() == 2);

    // Zero localities is a degenerate but legal table.
    slit->locality_count = 0;
    auto empty = acpi::numa::DistanceMatrix::from_table(*slit);
    require(bool(empty));
    require(empty->count() == 0);
}

/// The premise the kernel's own clamp rests on.
///
/// kernel/arch/x86/mm/srat.cpp reads only the leading MAX_NODES-by-MAX_NODES
/// square of a SLIT that declares more localities than the kernel can hold.
/// That is only sound because this matrix indexes with the *declared* stride,
/// so the leading square holds those nodes' true mutual distances rather than
/// a reshaped slice of the payload. It is also only necessary because this
/// library accepts such a table: it validates the count against the payload
/// and knows nothing about the kernel's node capacity, which is the right
/// division of labour and the reason the clamp lives on the other side.
///
/// Pinned here so that a later change to either half shows up as a failure
/// rather than as silently wrong distances.
auto test_oversized_count_is_accepted_with_a_readable_leading_square() -> void {
    Machine machine;

    // 40 localities -- more than the kernel's MAX_NODES of 32 -- with a payload
    // that genuinely holds all 1600 entries, so nothing here is a lying count.
    constexpr u32 COUNT = 40;
    auto const length = sizeof(acpi::slit::Table) + COUNT * COUNT;
    auto *slit = machine.add_table<acpi::slit::Table>(acpi::slit::Table::SIGNATURE, length);
    slit->locality_count = COUNT;
    for (u32 i = 0; i < COUNT; ++i) {
        for (u32 j = 0; j < COUNT; ++j) {
            slit->entry[i * COUNT + j] = u8(i == j ? 10 : 11 + ((i * 7 + j) % 200));
        }
    }
    machine.finish();

    auto matrix = acpi::numa::DistanceMatrix::from_table(*slit);
    require(bool(matrix));
    // Accepted, not clamped: this library does not know MAX_NODES.
    require(matrix->count() == COUNT);

    // Every entry of the leading 32x32 square reads back what was written at
    // the full stride, which is what makes reading only that square correct.
    for (u32 i = 0; i < 32; ++i) {
        for (u32 j = 0; j < 32; ++j) {
            require(matrix->distance(i, j) == u8(i == j ? 10 : 11 + ((i * 7 + j) % 200)));
        }
    }

    // And the rows past it are still there, so the truncation is the kernel's
    // choice rather than something this matrix imposed.
    require(matrix->distance(39, 39) == 10);
}

auto test_truncated_slit_is_rejected() -> void {
    // A SLIT whose declared length does not even cover locality_count.  Going
    // through find<> means this never reaches DistanceMatrix, but the matrix
    // refuses it on its own too.
    std::vector<u8> bytes(sizeof(acpi::slit::Table), 0);
    auto *slit = reinterpret_cast<acpi::slit::Table *>(bytes.data());
    acpi::test::make_table_header(slit, acpi::slit::Table::SIGNATURE,
                                  u32(sizeof(acpi::slit::Table) - 4));
    require(!acpi::numa::DistanceMatrix::from_table(*slit));
}

} // namespace

auto main() -> int {
    test_memory_regions();
    test_cpu_bindings_skip_disabled();
    test_distance_matrix();
    test_distance_matrix_rejects_lying_count();
    test_oversized_count_is_accepted_with_a_readable_leading_square();
    test_truncated_slit_is_rejected();
    return 0;
}
