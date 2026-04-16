#include <acpi/parser.hpp>

namespace acpi {
namespace {
    auto calc_checksum(void const *buffer, usize len) -> u8 {
        u8 sum = 0;
        for (auto i = 0; i < len; ++i) {
            sum += reinterpret_cast<char const *>(buffer)[i];
        }

        return sum;
    }

    template <typename T>
    auto map_structure(IPhysToVirt *p2v, PhysAddr phys_addr) -> ustl::Result<T const *, Status> {
        auto result = p2v->phys_to_virt(phys_addr, sizeof(T));
        if (!result) {
            return result.err();
        }

        auto header = static_cast<T const *>(result.unwrap());
        if (header->size() < sizeof(T)) {
            return ustl::err(Status::InternalError);
        }

        result = p2v->phys_to_virt(phys_addr, header->size());
        if (!result) {
            result.err();
        }
        return ustl::ok(static_cast<T const *>(result.unwrap()));
    }

    struct RootSystemTableDetails {
        PhysAddr rsdp_addr;
        PhysAddr rsdt_addr;
        PhysAddr xsdt_addr;
    };

    auto get_root_tables(IPhysToVirt *p2v, PhysAddr rsdp_pa) -> ktl::Result<RootSystemTableDetails> {
        auto maybe_rsdp = p2v->phys_to_virt(rsdp_pa, sizeof(sdt::Rsdp));
        if (!maybe_rsdp) {
            return maybe_rsdp.err();
        }

        auto rsdp = static_cast<sdt::Rsdp const *>(maybe_rsdp.unwrap());
        if (!rsdp->validate()) {
            return ktl::err(Status::NotFound);
        }

        RootSystemTableDetails rstd;
        rstd.rsdp_addr = rsdp_pa;
        rstd.rsdt_addr = rsdp->rsdt_addr;
        if (rsdp->revision >= 2) {
            auto maybe_xsdp = map_structure<sdt::RsdpV2>(p2v, rsdp_pa);
            if (!maybe_xsdp) {
                return maybe_xsdp.err();
            }

            auto xsdp = maybe_xsdp.unwrap();
            if (!xsdp->validate()) {
                return ktl::err(Status::NotFound);
            }

            rstd.xsdt_addr = xsdp->xsdt_addr;
        } else {
            rstd.xsdt_addr = 0;
        }

        return ustl::ok(rstd);
    }
} // namespace anonymous

    auto sdt::Rsdp::validate() const -> bool {
        if (!validate_signature()) {
            return false;
        }
        if (!validate_checksum(this, sizeof(*this))) {
            return false;
        }

        return true;
    }

    auto sdt::RsdpV2::validate() const -> bool {
        if (!v1.validate_signature()) {
            return false;
        }
        if (!validate_checksum(this, size())) {
            return false;
        }

        return true;
    }

    auto AcpiParser::from_rsdp(IPhysToVirt *p2v, PhysAddr rsdp_pa) -> ktl::Result<AcpiParser> {
        auto result = get_root_tables(p2v, rsdp_pa);
        if (!result) {
            return result.err();
        }

        // At first, we are preferred the more new version.
        if (result->xsdt_addr) {
            auto maybe_xsdt = p2v->phys_to_virt(result->xsdt_addr, sizeof(sdt::Xsdt));
            if (!maybe_xsdt) {
                return maybe_xsdt.err();
            }
            auto xsdt = static_cast<sdt::Xsdt const *>(maybe_xsdt.unwrap());
            if (validate_table(xsdt)) {
                auto const num_tables = xsdt->header.length / sizeof(u64);
                return ktl::ok(AcpiParser(p2v, result->rsdp_addr, xsdt, 0, rsdp_pa, num_tables));
            }
        }

        if (result->rsdt_addr) {
            auto maybe_rsdt = p2v->phys_to_virt(result->rsdt_addr, sizeof(sdt::Rsdt));
            if (!maybe_rsdt) {
                return maybe_rsdt.err();
            }
            auto rsdt = static_cast<sdt::Rsdt const *>(maybe_rsdt.unwrap());
            if (validate_table(rsdt)) {
                auto const num_tables = rsdt->header.length / sizeof(u32);
                return ktl::ok(AcpiParser(p2v, result->rsdp_addr, 0, rsdt, rsdp_pa, num_tables));
            }
        }

        return ktl::err(Status::NotFound);
    }

    auto AcpiParser::get_table_phys_addr(usize index) const -> PhysAddr {
        if (index < m_num_tables) {
            if (m_xsdt) {
                return m_xsdt->addr64[index];
            } 
            return m_rsdt->addr32[index];
        }
        return 0;
    }

    auto AcpiParser::get_table_at_index(usize index) const -> ktl::Result<TableHeader const *> {
        PhysAddr phys_addr = get_table_phys_addr(index);
        if (!phys_addr) {
            return ustl::err(Status::NotFound);
        }

        return map_structure<TableHeader>(m_p2v, phys_addr);
    }

    auto get_table_by_signature(IAcpiParser const &parser, Signature signature) -> TableHeader const * {
        auto const n = parser.num_tables();
        for (auto i = 0; i < n; ++i) {
            auto result = parser.get_table_at_index(i);
            if (!result) {
                continue;
            }

            if ((*result)->sign != signature) {
                continue;
            }

            if (validate_checksum(*result, (*result)->length)) {
                return result.unwrap();
            }
        }

        return 0;
    }

} // namespace acpi