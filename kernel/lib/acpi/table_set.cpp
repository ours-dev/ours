/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <acpi/table_set.hpp>

namespace acpi {
auto calc_checksum(void const *buffer, usize len) -> u8 {
    auto sum = u8(0);
    auto const *bytes = reinterpret_cast<u8 const *>(buffer);
    for (auto i = usize(0); i < len; ++i) {
        sum += bytes[i];
    }

    return sum;
}

auto sdt::Rsdp::validate() const -> bool {
    if (!validate_signature()) {
        return false;
    }

    return validate_checksum(this, sizeof(*this));
}

auto sdt::RsdpV2::validate() const -> bool {
    if (!v1.validate_signature()) {
        return false;
    }
    if (size() < sizeof(*this)) {
        return false;
    }
    // ACPI requires both checksums to hold: the 1.0 one over the leading 20
    // bytes, and this extended one over all `length` bytes.  Callers reach here
    // through get_root_tables(), which has already checked the former.
    return validate_checksum(this, size());
}

namespace {
    /// Maps a self-describing structure: first enough of it to read the length
    /// out of the header, then the whole declared extent.
    template <typename T>
    auto map_structure(PhysMapper &mapper, PhysAddr phys_addr) -> ktl::Result<T const *> {
        auto result = mapper.map(phys_addr, sizeof(T));
        if (!result) {
            return ktl::err(result.error());
        }

        auto const *partial = static_cast<T const *>(result.unwrap());
        if (partial->size() < sizeof(T)) {
            return ktl::err(Status::InvalidArgument);
        }

        result = mapper.map(phys_addr, partial->size());
        if (!result) {
            return ktl::err(result.error());
        }

        return ktl::ok(static_cast<T const *>(result.unwrap()));
    }

    struct RootTables {
        PhysAddr rsdt_addr;
        PhysAddr xsdt_addr;
    };

    auto get_root_tables(PhysMapper &mapper, PhysAddr rsdp_pa) -> ktl::Result<RootTables> {
        auto maybe_rsdp = mapper.map(rsdp_pa, sizeof(sdt::Rsdp));
        if (!maybe_rsdp) {
            return ktl::err(maybe_rsdp.error());
        }

        auto const *rsdp = static_cast<sdt::Rsdp const *>(maybe_rsdp.unwrap());
        if (!rsdp->validate()) {
            return ktl::err(Status::NotFound);
        }

        RootTables roots{.rsdt_addr = rsdp->rsdt_addr, .xsdt_addr = 0};
        if (rsdp->revision >= sdt::Rsdp::kRevisionV2) {
            auto maybe_xsdp = map_structure<sdt::RsdpV2>(mapper, rsdp_pa);
            if (!maybe_xsdp) {
                // An RSDP that claims revision 2 but whose extended part cannot
                // be mapped is not fatal; the 1.0 RSDT pointer is still good.
                return ktl::ok(roots);
            }

            auto const *xsdp = maybe_xsdp.unwrap();
            if (xsdp->validate()) {
                roots.xsdt_addr = PhysAddr(xsdp->xsdt_addr);
            }
        }

        return ktl::ok(roots);
    }

    /// Validates a root table: right signature, long enough, checksum holds,
    /// and a payload that divides evenly into pointers.
    template <typename Root>
    auto count_root_entries(Root const *root, usize pointer_size) -> usize {
        if (root->header.sign != Root::kSignature) {
            return 0;
        }
        if (root->header.length < sizeof(Root)) {
            return 0;
        }
        if (!validate_checksum(root, root->header.length)) {
            return 0;
        }

        auto const payload = usize(root->header.length) - sizeof(Root);
        if (payload % pointer_size != 0) {
            return 0;
        }

        return payload / pointer_size;
    }
} // namespace

auto TableSet::from_rsdp(PhysMapper &mapper, PhysAddr rsdp_pa) -> ktl::Result<TableSet> {
    auto roots = get_root_tables(mapper, rsdp_pa);
    if (!roots) {
        return ktl::err(roots.error());
    }

    // Prefer the XSDT: its 64-bit pointers can reach tables an RSDT cannot
    // describe.  Fall back to the RSDT whenever the XSDT is absent, unmappable
    // or malformed -- the old code fell back on a failed *validation* but bailed
    // out entirely on a failed *mapping*, which are the same situation as far as
    // a caller is concerned.
    if (roots->xsdt_addr) {
        if (auto maybe_xsdt = map_structure<sdt::Xsdt>(mapper, roots->xsdt_addr)) {
            auto const *xsdt = maybe_xsdt.unwrap();
            if (auto const count = count_root_entries(xsdt, sizeof(u64))) {
                return ktl::ok(TableSet(&mapper, nullptr, xsdt, count));
            }
        }
    }

    if (roots->rsdt_addr) {
        if (auto maybe_rsdt = map_structure<sdt::Rsdt>(mapper, roots->rsdt_addr)) {
            auto const *rsdt = maybe_rsdt.unwrap();
            if (auto const count = count_root_entries(rsdt, sizeof(u32))) {
                return ktl::ok(TableSet(&mapper, rsdt, nullptr, count));
            }
        }
    }

    return ktl::err(Status::NotFound);
}

auto TableSet::phys_addr_at(usize index) const -> PhysAddr {
    if (index >= m_num_tables) {
        return 0;
    }
    if (m_xsdt) {
        return PhysAddr(m_xsdt->addr64[index]);
    }

    return PhysAddr(m_rsdt->addr32[index]);
}

auto TableSet::header_at(usize index) -> ktl::Result<TableHeader const *> {
    auto const phys_addr = phys_addr_at(index);
    if (!phys_addr) {
        return ktl::err(Status::NotFound);
    }

    return map_structure<TableHeader>(*m_mapper, phys_addr);
}

auto TableSet::scan_for(Signature sign) -> TableHeader const * {
    for (auto i = usize(0); i < m_num_tables; ++i) {
        auto maybe_header = header_at(i);
        if (!maybe_header) {
            continue;
        }

        auto const *header = maybe_header.unwrap();
        if (header->sign != sign) {
            continue;
        }
        if (!validate_checksum(header, header->length)) {
            continue;
        }

        return header;
    }

    return nullptr;
}

auto TableSet::find_checked(Signature sign, usize min_size) -> TableHeader const * {
    // The cache is keyed on the signature alone, so the size check has to stay
    // out of the cached value; that also keeps two lookups of the same
    // signature through different structure types from poisoning each other.
    TableHeader const *header = nullptr;
    if (auto const *slot = cached(sign)) {
        header = slot->header;
    } else {
        header = scan_for(sign);
        remember(sign, header);
    }

    // The length check is the whole point of routing every lookup through here:
    // a table whose declared length is shorter than the structure the caller is
    // about to read must not be handed out.  header_at() has already mapped
    // exactly header->length bytes, so this bounds every field the caller can
    // reach through the returned pointer.
    if (!header || header->length < min_size) {
        return nullptr;
    }

    return header;
}

auto TableSet::cached(Signature sign) const -> CacheSlot const * {
    // Returns the slot rather than the pointer inside it, so that a remembered
    // "this table is absent" stays distinguishable from a cache miss.
    for (auto i = usize(0); i < m_cache_len; ++i) {
        if (m_cache[i].sign == sign) {
            return &m_cache[i];
        }
    }

    return nullptr;
}

auto TableSet::remember(Signature sign, TableHeader const *header) -> void {
    if (m_cache_len < kCacheSlots) {
        m_cache[m_cache_len] = CacheSlot{.sign = sign, .header = header};
        ++m_cache_len;
    }
}

} // namespace acpi
