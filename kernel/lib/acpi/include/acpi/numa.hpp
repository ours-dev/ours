/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_NUMA_HPP
#define ACPI_NUMA_HPP 1

#include <acpi/structures/slit.hpp>
#include <acpi/structures/srat.hpp>
#include <acpi/table_set.hpp>

namespace acpi::numa {
struct MemoryRegion {
    u32 domain;
    u64 base;
    u64 size;
};

/// One processor's affinity, with the APIC and x2APIC spellings unified.
struct CpuBinding {
    u32 apic_id;
    u32 domain;
};

/// A SLIT whose declared locality count has been checked to fit inside the
/// table, so that indexing it cannot run off the end.
///
/// Reading `slit::Table::locality_count` used to happen before any length
/// check -- in one place the check came on the following line, in another there
/// was none at all -- which let a SLIT whose declared length stopped short of
/// the count field be read past its mapping.  Making the validated matrix the
/// only way to touch the payload removes the opportunity.
class DistanceMatrix {
    using Self = DistanceMatrix;

  public:
    static auto from_table(slit::Table const &table) -> ktl::Result<DistanceMatrix>;

    [[nodiscard]]
    auto count() const -> u32 {
        return m_count;
    }

    /// Distance from one proximity domain to another.  Both indices must be
    /// below `count()`.
    [[nodiscard]]
    auto distance(u32 from, u32 to) const -> u8;

  private:
    constexpr DistanceMatrix(slit::Table const *table, u32 count)
        : m_table(table), m_count(count) {}

    slit::Table const *m_table{nullptr};
    u32 m_count{0};
};

/// Visits every *enabled* memory affinity entry in the SRAT.
///
/// Returns InternalError if the entry walk hit a malformed entry, Ok otherwise.
template <typename Visitor>
auto for_each_memory_region(srat::Table const &table, Visitor &&visit) -> Status {
    auto range = entries<srat::MemoryAffinityEntry>(table);
    for (auto const &entry: range) {
        if (!entry.enabled()) {
            continue;
        }

        visit(MemoryRegion{
            .domain = entry.proximity_domain,
            .base = entry.base_address(),
            .size = entry.length(),
        });
    }

    if (range.malformed()) {
        return Status::InternalError;
    }

    return Status::Ok;
}

/// Visits every *enabled* processor affinity entry in the SRAT, in both its
/// APIC (type 0x00) and x2APIC (type 0x02) forms.
template <typename Visitor>
auto for_each_cpu_binding(srat::Table const &table, Visitor &&visit) -> Status {
    auto apics = entries<srat::ProcessorAffinityEntry>(table);
    for (auto const &entry: apics) {
        // A disabled processor is a slot firmware described but did not
        // populate: hot-plug capacity, or the vCPUs between `-smp N` and
        // `maxcpus=M`.  Skipping it is what the enabled bit is for.  This used
        // to return InternalError instead, which failed the whole CPU-to-node
        // binding on any machine that declared a spare socket.
        if (!entry.enabled()) {
            continue;
        }

        visit(CpuBinding{.apic_id = entry.apic_id, .domain = entry.proximity_domain()});
    }

    auto x2apics = entries<srat::ProcessorX2ApicAffinityEntry>(table);
    for (auto const &entry: x2apics) {
        if (!entry.enabled()) {
            continue;
        }

        visit(CpuBinding{.apic_id = entry.x2apic_id, .domain = entry.proximity_domain});
    }

    if (apics.malformed() || x2apics.malformed()) {
        return Status::InternalError;
    }

    return Status::Ok;
}

} // namespace acpi::numa

#endif // #ifndef ACPI_NUMA_HPP
