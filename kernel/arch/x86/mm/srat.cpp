#include <ours/mm/early_mem.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/platform/acpi.hpp>

#include <acpi/numa.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/object.hpp>

namespace ours {
namespace {
    auto print_numa_info(acpi::srat::Table const &srat) -> void {
        log::trace("NUMA information from ACPI-SRAT");
        log::trace("|{} | {:18} | {:18}", "Domain", "Base", "Size");
        auto const status =
            acpi::numa::for_each_memory_region(srat, [](acpi::numa::MemoryRegion const &region) {
                log::trace("|{:6} | 0x{:16X} | 0x{:16X}", region.domain, region.base, region.size);
            });

        if (status != Status::Ok) {
            log::trace("No NUMA");
        }
    }

    auto register_meminfo(acpi::srat::Table const &srat) -> Status {
        auto assignment_status = Status::Ok;
        auto const status = acpi::numa::for_each_memory_region(
            srat, [&assignment_status](acpi::numa::MemoryRegion const &region) {
                log::trace("|{:6} | 0x{:16X} | 0x{:16X}", region.domain, region.base, region.size);
                if (assignment_status == Status::Ok && region.domain < NUMA_MAX_NODE) {
                    // Not all of machine are NUMA architecture. To support both NUMA and non-NUMA
                    // machine simultaneously, we just register affinity infomation of memory about
                    // NUMA.
                    assignment_status =
                        mm::EarlyMem::set_node(region.base, region.size, NnId{region.domain});
                    if (assignment_status == Status::Ok) {
                        mm::PmNode::set_state(NnId{region.domain}, mm::PmNode::State::Possible);
                    }
                }
            });
        if (status != Status::Ok) {
            return status;
        }

        return assignment_status;
    }

    auto reset_all_distances() -> void {
        for (auto i = 0; i < NUMA_MAX_NODE; ++i) {
            for (auto j = 0; j < NUMA_MAX_NODE; ++j) {
                mm::PmNode::Topology::set_distance(i, j, 0);
            }
        }
    }

    auto register_nodedis(acpi::TableSet &tables) -> Status {
        auto const *slit = tables.find<acpi::slit::Table>();
        if (!slit) {
            log::info("No NUMA");
            reset_all_distances();
            return Status::NotFound;
        }

        // The matrix validates the declared locality count against the table
        // length up front, so indexing it below cannot run off the end.
        auto matrix = acpi::numa::DistanceMatrix::from_table(*slit);
        if (!matrix) {
            log::info("No NUMA");
            reset_all_distances();
            return matrix.error();
        }

        auto const count = matrix->count();
        for (auto from = u32(0); from < count; ++from) {
            for (auto to = u32(0); to < count; ++to) {
                mm::PmNode::Topology::set_distance(from, to, matrix->distance(from, to));
            }
        }

        return Status::Ok;
    }

    /// FIXME(SmallHuaZi) Temporary mapping to forward development in CpuLocal.
    /// It should be the content of topology.
    auto bind_node_to_cpu(acpi::srat::Table const &srat) -> Status {
        INIT_DATA
        static usize APICIDS[MAX_CPU];

        INIT_DATA
        static usize CPUS[MAX_CPU];

        usize count = 0;
        return acpi::numa::for_each_cpu_binding(srat, [&](acpi::numa::CpuBinding const &binding) {
            if (count >= MAX_CPU) {
                return;
            }
            APICIDS[count] = binding.apic_id;
            CPUS[count] = count;
            cpu_possible_mask().set(count);
            // HINT(SmallHuaZi)
            // set_cpu_node_early(), not set_nid(): this runs at PrePmm + 1, before
            // any PmNode exists. set_nid() is the run-time move between two
            // live nodes -- it finds no node to join, so it wrote only the
            // dynamic table and left the firmware table on the sentinel.
            // bootstrap() then saw the CPU as already claimed and did not
            // fill it either, and PmNode::init(), which builds each node's
            // hard affinity from the static table, built them all empty.
            mm::PmNode::Topology::set_cpu_node_early(static_cast<CpuNum>(count),
                                                     NnId{binding.domain});
            count += 1;
        });
    }
} // namespace

// The strong override of numa.cpp's WEAK_LINK stub: ACPI-SRAT discovery is
// the x86 way to learn the NUMA topology, tried before the PCI fallback.
auto acpi_init_numa() -> Status {
    auto tables = get_acpi_tables();
    DEBUG_ASSERT(tables, "");
    if (!tables) {
        return Status::NotFound;
    }

    auto const *srat = tables->find<acpi::srat::Table>();
    if (!srat) {
        return Status::NotFound;
    }

    Status status = register_meminfo(*srat);
    if (status != Status::Ok) {
        return status;
    }

    // For debug
    print_numa_info(*srat);

    mm::EarlyMem::dump();

    status = register_nodedis(*tables);
    if (status != Status::Ok) {
        return status;
    }

    status = bind_node_to_cpu(*srat);
    if (status != Status::Ok) {
        return status;
    }

    return Status::Ok;
}

} // namespace ours
