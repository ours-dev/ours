#include <acpi/apic.hpp>

#include <acpi/structures/madt.hpp>

#include <ustl/function/fn.hpp>
#include <ustl/io/binary_reader.hpp>

namespace acpi {
namespace madt {
    template <typename Entry, typename Visitor>
    FORCE_INLINE
    auto enumerate_entries(IAcpiParser const &parser, Visitor visitor) -> Status {
        auto table = get_table_by_signature(parser, Table::kSignature);
        if (!table) {
            return Status::NotFound;
        }

        auto reader = ustl::io::BinaryReader::from_payload_of_struct(&table);
        while (reader) {
            auto header = reader.read<EntryHeader>();
            if (!header) {
                return Status::ParsingFailed;
            } else if (static_cast<Type>(header->type) != Entry::kType) {
                continue;
            }

            auto entry = downcast<Entry>(header);
            if (!entry) {
                return Status::ParsingFailed;
            }

            visitor(*entry);
        }

        return Status::Ok;
    }
} // namespace madt

    auto enumerate_io_apics(IAcpiParser const &parser, IoApicCommitFn const &commit) -> Status {
        return enumerate_entries<madt::IoApicEntry>(parser, commit);
    }

    auto enumerate_local_apics(IAcpiParser const &parser, LocalApicCommitFn const &commit) -> Status {
        return enumerate_entries<madt::LocalApicEntry>(parser, commit);
    }

    auto enumerate_interrupt_overrides(IAcpiParser const &parser, InterruptOverrideCommitFn const &commit) -> Status {
        return enumerate_entries<madt::InterruptOverrideEntry>(parser, commit);
    }

} // namespace acpi
