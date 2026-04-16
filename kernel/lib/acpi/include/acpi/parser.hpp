/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_PARSER_HPP
#define ACPI_PARSER_HPP 1

#include <acpi/types.hpp>
#include <acpi/structures/header.hpp>
#include <acpi/structures/rdst.hpp>

#include <ktl/result.hpp>

namespace acpi {
    class IPhysToVirt {
      public:
        virtual auto phys_to_virt(PhysAddr addr, usize size) -> ktl::Result<void const *> = 0;
      protected:
        IPhysToVirt() = default;
        virtual ~IPhysToVirt() = default;
    };

    class IAcpiParser {
      public:
        virtual auto num_tables() const -> usize = 0;
        virtual auto get_table_at_index(usize index) const -> ktl::Result<TableHeader const *> = 0;
      protected:
        IAcpiParser() = default;
        virtual ~IAcpiParser() = default;
    };

    class AcpiParser: public IAcpiParser {
        using Self = AcpiParser;
      public:
        static auto from_rsdp(IPhysToVirt *mapper, PhysAddr rsdp) -> ktl::Result<AcpiParser>;

        auto num_tables() const -> usize override {
            return m_num_tables;
        }

        auto get_table_at_index(usize index) const -> ktl::Result<TableHeader const *> override;
        auto get_table_phys_addr(usize index) const -> PhysAddr;

        AcpiParser() = default;
        virtual ~AcpiParser() = default;
      private:
        FORCE_INLINE CXX11_CONSTEXPR
        AcpiParser(IPhysToVirt *p2v, PhysAddr rsdp_addr, 
                   sdt::Xsdt const *xsdt, sdt::Rsdt const *rsdt,
                   PhysAddr root_table_paddr, usize num_tables)
            : m_p2v(p2v),
              m_rsdp_paddr(rsdp_addr),
              m_rsdt(rsdt),
              m_xsdt(xsdt),
              m_root_table_paddr(root_table_paddr),
              m_num_tables(num_tables) 
        {}

        IPhysToVirt *m_p2v;
        sdt::Rsdt const *m_rsdt;
        sdt::Xsdt const *m_xsdt;
        PhysAddr m_rsdp_paddr;
        PhysAddr m_root_table_paddr;
        usize m_num_tables;
    };

    auto calc_checksum(void const *buffer, usize len) -> u8;

    FORCE_INLINE CXX11_CONSTEXPR 
    auto validate_checksum(void const *buffer, usize len) -> bool {
        return 0 == calc_checksum(buffer, len);
    }

    template <typename Table>
    FORCE_INLINE 
    auto validate_table(Table *table) -> bool {
        if (table->header.sign != Table::kSignature) {
            return false;
        }
        if (!validate_checksum(table, table->header.size())) {
            return false;
        }

        return true;
    }

    auto get_table_by_signature(IAcpiParser const &, Signature) -> TableHeader const *;

    template <typename Table>
    auto get_table_by_type(IAcpiParser const &parser) -> Table const * {
        return reinterpret_cast<Table const *>(get_table_by_signature(parser, Table::kSignature));
    }

} // namespace acpi

#endif // #ifndef ACPI_PARSER_HPP