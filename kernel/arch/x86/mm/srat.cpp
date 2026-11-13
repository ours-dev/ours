#include <ours/mm/early_mem.hpp>
#include <ours/mm/node_table.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/platform/acpi.hpp>

#include <algorithm>

#include <acpi/numa.hpp>
#include <logz4/log.hpp>
#include <numa/topology.hpp>
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
            log::trace("numa: the SRAT's memory-region walk ended early ({})", to_string(status));
        }
    }

    /// What register_meminfo() did, because a Status cannot say "some of it".
    ///
    /// The walk has three outcomes per region and one return value: the region
    /// is re-homed onto the domain the SRAT names, or it is left where it is
    /// because that domain id is one this kernel cannot hold, or bootmem
    /// refuses the range. The first two both used to leave the status Ok, so a
    /// boot that re-homed every region and a boot that re-homed none of them
    /// were indistinguishable -- to the caller and in the log.
    struct MemoryAffinityReport {
        u32 described{};
        u32 rehomed{};
        u32 truncated{};
    };

    auto register_meminfo(acpi::srat::Table const &srat, MemoryAffinityReport *const report)
        -> Status {
        auto failure = Status::Ok;
        auto const status = acpi::numa::for_each_memory_region(
            srat, [report, &failure](acpi::numa::MemoryRegion const &region) {
                log::trace("|{:6} | 0x{:16X} | 0x{:16X}", region.domain, region.base, region.size);
                report->described += 1;

                // There is nothing useful left to do once bootmem has refused a
                // range: the status carries that error out and the caller
                // abandons the topology. The walk cannot be stopped from in
                // here -- the library's visitor returns void -- so the
                // remaining regions are counted and skipped.
                if (failure != Status::Ok) {
                    return;
                }

                // Split from the latch above, which it used to share an `if`
                // with. Sharing one meant a domain id this kernel cannot
                // represent left the status Ok: the region silently kept node
                // 0 and the caller was told the SRAT had been applied whole.
                // The comment that used to stand here justified the skip by
                // non-NUMA machines, which is a domain id of 0 on a uniform
                // machine -- not a domain beyond the node ceiling, which is a
                // truncated topology and is reported as one.
                if (region.domain >= MAX_NODES) {
                    report->truncated += 1;
                    return;
                }

                failure = mm::EarlyMem::set_node(PhysAddr{region.base}, region.size,
                                                 NodeId{region.domain});
                if (failure != Status::Ok) {
                    log::error("numa: [0x{:X}, 0x{:X}) would not move to node {} ({})", region.base,
                               region.base + region.size, region.domain, to_string(failure));
                    return;
                }

                report->rehomed += 1;
                mm::set_node_state(NodeId{region.domain}, mm::NodeState::Possible);
            });

        return status != Status::Ok ? status : failure;
    }

    auto reset_all_distances() -> void {
        for (auto i = NodeId(0); i < MAX_NODES; ++i) {
            for (auto j = NodeId(0); j < MAX_NODES; ++j) {
                ::numa::Topology::set_distance(i, j, 0);
            }
        }
    }

    /// Fills the distance matrix from the ACPI SLIT, or leaves every node
    /// equidistant.
    ///
    /// No status, because there is nothing here for the caller to decide. A
    /// SLIT is an optional refinement of the proximity domains the SRAT
    /// declares: absent, malformed, or naming more localities than this kernel
    /// holds, the matrix is left in a state the fallback order handles, and
    /// each way out says in the log which one it was.
    ///
    /// It used to return one, and the caller read it as a verdict on the whole
    /// topology: a missing SLIT -- legal, and the usual shape on real hardware
    /// -- aborted acpi_init_numa() before it bound a single CPU to a node, so
    /// every node's hard affinity came out empty and the boot logged that no
    /// firmware topology had been found while the 32 nodes it had just brought
    /// up from the SRAT owned all of the memory. A function whose outcome no
    /// caller may act on should not hand one out.
    auto register_nodedis(acpi::TableSet &tables) -> void {
        auto const *slit = tables.find<acpi::slit::Table>();
        if (!slit) {
            // An SRAT without a SLIT is legal and common: the machine names its
            // proximity domains but not how far apart they are. Every distance
            // stays zero, so the fallback order degenerates to node-id order --
            // which is a decision, not a failure, and the log should say which
            // one was taken.
            log::info("numa: no ACPI SLIT; every node reads as equidistant");
            reset_all_distances();
            return;
        }

        // The matrix validates the declared locality count against the table
        // length up front, so indexing it below cannot run off the end.
        auto matrix = acpi::numa::DistanceMatrix::from_table(*slit);
        if (!matrix) {
            // Not fatal either, for the same reason: the domains the SRAT
            // named exist whatever the SLIT says about the distances between
            // them, and uniform distances are the honest answer when the table
            // that was supposed to give them cannot be read.
            log::info("numa: the ACPI SLIT was rejected ({})", to_string(matrix.error()));
            reset_all_distances();
            return;
        }

        // Said on the way through, not only on failure: a SLIT declaring no
        // localities is accepted by the matrix and leaves every distance zero,
        // so without the count in the log it is indistinguishable from the
        // table being absent.
        auto const declared = matrix->count();
        log::info("numa: the ACPI SLIT declares {} localities", declared);

        // Clamped here, because here is where the firmware's idea of how many
        // nodes exist meets this kernel's.
        //
        // Neither side can do it alone. kernel/lib/acpi validates the count
        // against the SLIT's own payload length and is right not to know
        // MAX_NODES -- it describes ACPI tables, not this kernel's capacity --
        // and Topology::set_distance() guards its array with a DEBUG_ASSERT,
        // which is `if constexpr (KERNEL_DEBUG_LEVEL)` and so compiles to
        // nothing in the dev image. The loop below therefore used to carry a
        // firmware-supplied count straight into an 8 KiB array with no check
        // anywhere in the image that boots by default: a SLIT declaring more
        // than MAX_NODES localities wrote past its end, by as much as the
        // count squared.
        auto const usable = u32(std::min<usize>(declared, MAX_NODES));
        if (usable < declared) {
            // Truncating rather than refusing the table, which is a deliberate
            // departure -- see docs/changes/261029/mm.md. The kept distances
            // are correct: distance(from, to) indexes with the *declared*
            // stride, so reading the leading usable-by-usable square of the
            // payload gives those nodes' true mutual distances. Refusing would
            // throw that away and leave the nodes we do have in node-id order.
            //
            // Warned rather than logged, because the fallback orders that come
            // out of this will be missing nodes, and a reader puzzling over
            // them has to be able to find the reason.
            log::warn("numa: using {} of them; this kernel holds {} nodes", usable, MAX_NODES);
        }

        for (auto from = u32(0); from < usable; ++from) {
            for (auto to = u32(0); to < usable; ++to) {
                ::numa::Topology::set_distance(from, to, matrix->distance(from, to));
            }
        }
    }

    /// FIXME(SmallHuaZi) Temporary mapping to forward development in CpuLocal.
    /// It should be the content of topology.
    auto bind_node_to_cpu(acpi::srat::Table const &srat) -> Status {
        INIT_DATA
        static usize APICIDS[MAX_CPU];

        INIT_DATA
        static usize CPUS[MAX_CPU];

        usize count = 0;
        auto const status =
            acpi::numa::for_each_cpu_binding(srat, [&](acpi::numa::CpuBinding const &binding) {
                if (count >= MAX_CPU) {
                    return;
                }
                APICIDS[count] = binding.apic_id;
                CPUS[count] = count;
                cpu_possible_mask().set(count);
                // set_firmware_node(), not PmNode::set_cpu_node(): this runs at
                // PrePmm + 1, before any PmNode exists, and recording what the
                // firmware declared is all there is to do this early.
                // set_cpu_node() is the run-time move between two live nodes --
                // it would find no node to join, so it wrote only the running
                // placement and left the firmware table on the sentinel.
                // bootstrap() then saw the CPU as already claimed and did not
                // fill it either, and PmNode::init(), which builds each node's
                // hard affinity from the firmware table, built them all empty.
                ::numa::Topology::set_firmware_node(static_cast<CpuNum>(count),
                                                    NodeId{binding.domain});
                count += 1;
            });

        // Zero bindings is otherwise a silent success: the walk simply never
        // calls back, every entry of the firmware placement table keeps its
        // sentinel, and the only trace of it is pmnode.test reporting that no
        // CPU was placed by firmware -- with nothing anywhere saying why.
        log::info("numa: the ACPI SRAT places {} CPU(s)", count);
        return status;
    }
} // namespace

// The strong override of numa.cpp's WEAK_LINK stub: ACPI-SRAT discovery is
// the x86 way to learn the NUMA topology, tried before the PCI fallback.
auto acpi_init_numa() -> Status {
    auto tables = get_acpi_tables();
    DEBUG_ASSERT(tables, "");
    if (!tables) {
        log::info("numa: no ACPI tables, so no SRAT to read");
        return Status::NotFound;
    }

    auto const *srat = tables->find<acpi::srat::Table>();
    if (!srat) {
        // The ordinary case on an ordinary machine, so this is information
        // rather than a complaint. It is also the line whose absence made a
        // uniform-memory boot look like a broken NUMA boot.
        log::info("numa: no ACPI SRAT; the machine declares no proximity domains");
        return Status::NotFound;
    }

    MemoryAffinityReport memory{};
    auto const status = register_meminfo(*srat, &memory);
    // Said on the way through, not only on failure: how many regions moved is
    // the only thing that tells a complete re-homing from a partial one, and
    // the status cannot carry it.
    log::info("numa: the ACPI SRAT describes {} memory region(s), {} re-homed", memory.described,
              memory.rehomed);
    if (memory.truncated) {
        // Warned, because those regions keep node 0 while their neighbours
        // move, and a reader wondering why one node holds memory the SRAT
        // gave to another has to be able to find the reason.
        log::warn("numa: {} of them name a domain beyond the {} nodes this kernel holds,"
                  " so that memory stays on node 0",
                  memory.truncated, MAX_NODES);
    }
    if (status != Status::Ok) {
        log::info("numa: the SRAT's memory affinities were not usable ({})", to_string(status));
        return status;
    }

    // For debug
    print_numa_info(*srat);

    mm::EarlyMem::dump();

    // Everything above decides whether this machine has a topology; everything
    // below refines one that already exists. That is why neither of the two
    // stages left can fail this function.
    //
    // The two are not symmetric in what they leave behind, and neither is
    // recoverable by returning early. register_nodedis() always leaves the
    // matrix readable, so a boot with no SLIT differs from one with a SLIT
    // only in the order its fallback chains come out. bind_node_to_cpu() can
    // stop part-way through a malformed entry list, and the CPUs it never
    // reached are then unknown to the topology altogether: not in the
    // possible mask, so bootstrap() does not see them, and with no firmware
    // node, so no node's hard affinity will ever hold them. The MADT reports
    // them later and claim_possible_cpus() puts them on the boot CPU's node,
    // which is somewhere to run and not necessarily where they are. That is
    // a degraded topology, and still better than discarding one whose memory
    // affinities are already applied and cannot be rolled back, and then
    // reporting that no firmware topology was found.
    register_nodedis(*tables);

    if (auto const binding = bind_node_to_cpu(*srat); binding != Status::Ok) {
        log::warn("numa: the SRAT's CPU affinity list ended early ({}); the CPUs it did name"
                  " are placed",
                  to_string(binding));
    }

    return Status::Ok;
}

} // namespace ours
