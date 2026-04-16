/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_NUMA_HPP
#define ACPI_NUMA_HPP 1

#include <acpi/parser.hpp>
#include <acpi/structures/srat.hpp>
#include <acpi/structures/slit.hpp>

#include <ustl/function/fn.hpp>

namespace acpi {
    struct NumaRegion {
        u64 base;
        u64 size;
    };

    using NumaRegionCommitFn = ustl::function::Fn<void(u32 numa_domain, NumaRegion const &)>;
    auto enumerate_numa_region(IAcpiParser const &parser, NumaRegionCommitFn const &) -> Status;

    using CpuNumaPairsCommitFn = ustl::function::Fn<void(u32 cpunum, u32 numa_domain)>;
    auto enumerate_cpu_numa_pairs(IAcpiParser const &parser, CpuNumaPairsCommitFn const &) -> Status;

    using NumaDistanceCommitFn = ustl::function::Fn<void(u32 from, u32 to, u8 distance)>;
    auto enumerate_numa_domain_distance(IAcpiParser const &parser, NumaDistanceCommitFn const &) -> Status;

    auto count_numa_domains(IAcpiParser const &parser) -> usize;

} // namespace acpi

#endif // #ifndef ACPI_NUMA_HPP