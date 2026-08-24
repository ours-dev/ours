/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_TEST_ACPI_TEST_BLOB_HPP
#define ACPI_TEST_ACPI_TEST_BLOB_HPP 1

#include <acpi/structures/madt.hpp>
#include <acpi/structures/root.hpp>
#include <acpi/structures/slit.hpp>
#include <acpi/structures/srat.hpp>
#include <acpi/table_set.hpp>

#include <vector>

#include <cassert>
#include <cstdio>
#include <cstring>

/// Builds ACPI table images in host memory and serves them through a synthetic
/// physical address space.
///
/// The whole library is byte decoding behind a single seam -- acpi::PhysMapper
/// -- so a plain host program can drive every path in it.  That is what keeping
/// the mapper virtual, and everything else not, buys.
namespace acpi::test {

inline auto require(bool condition, char const *expr = "", char const *file = __builtin_FILE(),
                    unsigned line = __builtin_LINE()) -> void {
    if (!condition) {
        std::fprintf(stderr, "%s:%u: requirement failed %s\n", file, line, expr);
        std::abort();
    }
}

/// Translates synthetic physical addresses to the host buffers backing them.
///
/// Deliberately not an identity mapper.  Table pointers inside an RSDT are 32
/// bits wide, so identity-mapping host heap addresses would truncate them; and
/// a translating mapper that knows each region's extent will refuse a read that
/// runs past a table, which turns any over-read in the library into a visible
/// failure instead of a silently successful read of neighbouring heap.
class HostMapper final: public PhysMapper {
  public:
    struct Region {
        PhysAddr base;
        u8 const *host;
        usize size;
    };

    auto add_region(PhysAddr base, u8 const *host, usize size) -> void {
        regions_.push_back(Region{base, host, size});
    }

    auto map(PhysAddr addr, usize size) -> ktl::Result<void const *> override {
        ++calls;
        if (fail_all) {
            return ktl::err(Status::InternalError);
        }

        for (auto const &region: regions_) {
            if (addr < region.base || addr - region.base >= region.size) {
                continue;
            }

            auto const offset = usize(addr - region.base);
            if (region.size - offset < size) {
                return ktl::err(Status::OutOfRange);
            }

            return ktl::ok(static_cast<void const *>(region.host + offset));
        }

        return ktl::err(Status::NotFound);
    }

    usize calls{0};
    bool fail_all{false};

  private:
    std::vector<Region> regions_;
};

/// One table image, at a fixed synthetic physical base.  Each lives in its own
/// allocation so that addresses stay put as more tables are added.
struct Blob {
    Blob(PhysAddr base, usize size) : bytes(size, 0), base(base) {}

    [[nodiscard]]
    auto phys() const -> PhysAddr {
        return base;
    }

    [[nodiscard]]
    auto data() -> u8 * {
        return bytes.data();
    }

    template <typename T>
    [[nodiscard]]
    auto as() -> T * {
        return reinterpret_cast<T *>(bytes.data());
    }

    std::vector<u8> bytes;
    PhysAddr base;
};

inline auto fill_checksum(void *table, usize len) -> void {
    auto *bytes = static_cast<u8 *>(table);
    bytes[9] = 0; // TableHeader::checksum
    bytes[9] = u8(0u - calc_checksum(table, len));
}

inline auto make_table_header(void *table, Signature sign, u32 length) -> void {
    auto *header = static_cast<TableHeader *>(table);
    header->sign = sign;
    header->length = length;
    header->revision = 1;
    header->checksum = 0;
    std::memcpy(header->oem_id, "OURS  ", 6);
    std::memcpy(header->oem_table_id, "TESTTEST", 8);
    header->oem_revision = 1;
    header->creator_id = 0;
    header->creator_revision = 0;
    fill_checksum(table, length);
}

/// A complete RSDP + XSDT (and optionally RSDT) image plus the tables they
/// point at.
class Machine {
  public:
    /// Registers a table image and returns a mutable pointer to it.  The caller
    /// fills in the body afterwards, so the checksum is left for `finish()` to
    /// compute -- sealing here would just be undone by the caller's writes.
    template <typename Table>
    auto add_table(Signature sign, usize length) -> Table * {
        tables_.push_back(alloc(length));
        auto *raw = tables_.back()->data();
        make_table_header(raw, sign, u32(length));
        return reinterpret_cast<Table *>(raw);
    }

    /// Re-seals the checksum of the table at `index` after the caller mutated
    /// its body.
    auto reseal(usize index) -> void {
        auto *raw = tables_[index]->data();
        fill_checksum(raw, reinterpret_cast<TableHeader *>(raw)->length);
    }

    auto table_bytes(usize index) -> u8 * {
        return tables_[index]->data();
    }

    /// Builds the XSDT and the ACPI 2.0 RSDP that points at it.
    auto finish(u8 revision = sdt::Rsdp::kRevisionV2) -> void {
        // Seal every table now that the caller has finished writing its body.
        for (usize i = 0; i < tables_.size(); ++i) {
            reseal(i);
        }

        auto const xsdt_len = sizeof(sdt::Xsdt) + tables_.size() * sizeof(u64);
        xsdt_ = alloc(xsdt_len);
        auto *xsdt = xsdt_->as<sdt::Xsdt>();
        make_table_header(xsdt, sdt::Xsdt::kSignature, u32(xsdt_len));
        for (usize i = 0; i < tables_.size(); ++i) {
            auto const addr = u64(tables_[i]->phys());
            std::memcpy(xsdt_->data() + sizeof(sdt::Xsdt) + i * sizeof(u64), &addr, sizeof(addr));
        }
        fill_checksum(xsdt, u32(xsdt_len));

        auto const rsdt_len = sizeof(sdt::Rsdt) + tables_.size() * sizeof(u32);
        rsdt_ = alloc(rsdt_len);
        auto *rsdt = rsdt_->as<sdt::Rsdt>();
        make_table_header(rsdt, sdt::Rsdt::kSignature, u32(rsdt_len));
        for (usize i = 0; i < tables_.size(); ++i) {
            auto const addr = u32(tables_[i]->phys());
            std::memcpy(rsdt_->data() + sizeof(sdt::Rsdt) + i * sizeof(u32), &addr, sizeof(addr));
        }
        fill_checksum(rsdt, u32(rsdt_len));

        rsdp_ = alloc(sizeof(sdt::RsdpV2));
        auto *rsdp = rsdp_->as<sdt::RsdpV2>();
        rsdp->v1.sig_low = sdt::Rsdp::kSignatureLow;
        rsdp->v1.sig_high = sdt::Rsdp::kSignatureHigh;
        rsdp->v1.revision = revision;
        rsdp->v1.rsdt_addr = u32(rsdt_->phys());
        std::memcpy(rsdp->v1.oem_id, "OURS  ", 6);
        rsdp->length = u32(sizeof(sdt::RsdpV2));
        rsdp->xsdt_addr = u64(xsdt_->phys());
        seal_rsdp();

        for (auto const &blob: tables_) {
            mapper_.add_region(blob->phys(), blob->bytes.data(), blob->bytes.size());
        }
        for (auto const *blob: {xsdt_.get(), rsdt_.get(), rsdp_.get()}) {
            mapper_.add_region(blob->phys(), blob->bytes.data(), blob->bytes.size());
        }
    }

    /// Recomputes both RSDP checksums after the caller poked at it.
    auto seal_rsdp() -> void {
        auto *rsdp = rsdp_->as<sdt::RsdpV2>();
        rsdp->v1.checksum = 0;
        rsdp->v1.checksum = u8(0u - calc_checksum(&rsdp->v1, sizeof(sdt::Rsdp)));
        rsdp->ext_checksum = 0;
        rsdp->ext_checksum = u8(0u - calc_checksum(rsdp, rsdp->length));
    }

    [[nodiscard]]
    auto rsdp() -> sdt::RsdpV2 * {
        return rsdp_->as<sdt::RsdpV2>();
    }

    [[nodiscard]]
    auto rsdp_phys() const -> PhysAddr {
        return rsdp_->phys();
    }

    [[nodiscard]]
    auto xsdt() -> sdt::Xsdt * {
        return xsdt_->as<sdt::Xsdt>();
    }

    auto reseal_xsdt() -> void {
        fill_checksum(xsdt_->data(), xsdt()->header.length);
    }

    [[nodiscard]]
    auto mapper() -> HostMapper & {
        return mapper_;
    }

  private:
    /// Hands out synthetic physical addresses that fit in the 32 bits an RSDT
    /// entry has, and sizes each region to exactly one table.
    auto alloc(usize size) -> std::unique_ptr<Blob> {
        auto blob = std::make_unique<Blob>(next_phys_, size);
        next_phys_ += (size + 0xfff) & ~usize(0xfff);
        return blob;
    }

    PhysAddr next_phys_{0x10'0000};
    HostMapper mapper_;
    std::vector<std::unique_ptr<Blob>> tables_;
    std::unique_ptr<Blob> xsdt_;
    std::unique_ptr<Blob> rsdt_;
    std::unique_ptr<Blob> rsdp_;
};

} // namespace acpi::test

#endif // #ifndef ACPI_TEST_ACPI_TEST_BLOB_HPP
