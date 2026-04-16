#include <acpi/numa.hpp>

#include <ustl/io/binary_reader.hpp>
#include <ustl/traits/is_same.hpp>

namespace acpi {
    auto enumerate_numa_region(IAcpiParser const &parser, NumaRegionCommitFn const &commit) -> Status {
        using namespace srat;

        auto const header = get_table_by_signature(parser, srat::Table::kSignature);
        if (!header) {
            return Status::NotFound;
        }

        auto const srat = reinterpret_cast<srat::Table const *>(header);
        auto reader = ustl::io::BinaryReader::from_payload_of_struct(srat);
        while (reader) {
            auto header = reader.read<EntryHeader>();
            if (!header) {
                return Status::InternalError;
            }
            if (static_cast<Type>(header->type) != Type::MemoryAffinity) {
                continue;
            }

            auto memory = downcast<MemoryAffinityEntry>(header);
            if (!memory) {
                return Status::InternalError;
            }
            if (!(memory->flags & Flag::Enabled)) {
                continue;
            }

            NumaRegion region;
            region.base = memory->base_address_low | (u64(memory->base_address_high) << 32);
            region.size = memory->length_low | (u64(memory->length_high) << 32);
            commit(memory->proximity_domain, region);
        }

        return Status::Ok;
    }

    /// Parse SRAT to acquire the map between APICID and proximity domain(namely NUMA domain).
    auto enumerate_cpu_numa_pairs(IAcpiParser const &parser, CpuNumaPairsCommitFn const &commit) -> Status {
        using namespace srat;

        auto const header = get_table_by_signature(parser, srat::Table::kSignature);
        if (!header) {
            return Status::NotFound;
        }

        auto const srat = reinterpret_cast<srat::Table const *>(header);
        auto reader = ustl::io::BinaryReader::from_payload_of_struct(srat);
        while (reader) {
            auto header = reader.read<EntryHeader>();
            if (!header) {
                return Status::InternalError;
            }

            if (static_cast<Type>(header->type) == Type::ProcessorAffinity) {
                auto processor = downcast<ProcessorAffinityEntry>(header);
                if (!processor) {
                    return Status::InternalError;
                }
                if (!(processor->flags & Flag::Enabled)) {
                    return Status::InternalError;
                }
                commit(processor->apic_id, processor->proximity_domain());
            } else if (static_cast<Type>(header->type) == Type::ProcessorX2ApicAffinity) {
                auto processor = downcast<ProcessorX2ApicAffinityEntry>(header);
                if (!processor) {
                    return Status::InternalError;
                }
                if (!(processor->flags & Flag::Enabled)) {
                    return Status::InternalError;
                }
                commit(processor->x2apic_id, processor->proximity_domain);
            }
        }

        return Status::Ok;
    }

    auto count_numa_domains(IAcpiParser const &parser) -> usize {
        auto const header = get_table_by_signature(parser, slit::Table::kSignature);
        if (!header) {
            return 0;
        }

        auto slit = reinterpret_cast<slit::Table const *>(header);
        return slit->locality_count;
    }

    /// Parse SLIT to acquire the distance map among all proximity domains.
    auto enumerate_numa_domain_distance(IAcpiParser const &parser, NumaDistanceCommitFn const &commit) -> Status {
        auto const header = get_table_by_signature(parser, slit::Table::kSignature);
        if (!header) {
            return Status::NotFound;
        }

        auto const slit = reinterpret_cast<slit::Table const *>(header);
        auto const num_domain = slit->locality_count;
        for (u64 i = 0; i < num_domain; ++i) {
            for (u64 j = 0; j < num_domain; ++j) {
                commit(i, j, slit->entry[i * num_domain + j]);
            }
        }

        return Status::Ok;
    }

} // namespace acpi