/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/acpi_tables.hpp>

#include <cstring>

#include <acpi/structures/header.hpp>
#include <acpi_route/table_directory.hpp>

namespace acpi_route {
namespace {
    using ours::Status;

    constexpr auto align_up(usize const value) -> usize {
        constexpr auto MASK = ours::ACPI_TABLE_ALIGNMENT - 1;
        return (value + MASK) & ~MASK;
    }

    /// The entry `index` of a directory whose entries have been checked to lie
    /// inside `bytes`. Copied out, so that the bytes need no alignment.
    auto entry_at(std::span<u8 const> const bytes, u32 const index) -> ours::AcpiDirectoryEntry {
        ours::AcpiDirectoryEntry entry;
        std::memcpy(&entry,
                    bytes.data() + sizeof(ours::AcpiDirectory) +
                        usize{index} * sizeof(ours::AcpiDirectoryEntry),
                    sizeof(entry));
        return entry;
    }

    /// Whether the bytes add up to zero modulo 256, which is how ACPI checksums
    /// a table.
    auto sums_to_zero(std::span<u8 const> const table) -> bool {
        auto sum = u8{0};
        for (auto const byte: table) {
            sum = u8(sum + byte);
        }
        return sum == 0;
    }
} // namespace

auto TableDirectory::from_bytes(std::span<u8 const> const bytes)
    -> ustl::Result<TableDirectory, Status> {
    if (bytes.size() < sizeof(ours::AcpiDirectory)) {
        return ustl::err(Status::InvArgs);
    }
    ours::AcpiDirectory directory;
    std::memcpy(&directory, bytes.data(), sizeof(directory));
    if (directory.magic != ours::ACPI_DIRECTORY_MAGIC ||
        directory.version != ours::ACPI_DIRECTORY_VERSION) {
        return ustl::err(Status::InvArgs);
    }

    // The directory's own claim about its size is checked first, against the
    // bytes there really are, and everything after is checked against it.
    if (directory.size > bytes.size()) {
        return ustl::err(Status::InvArgs);
    }
    auto const entries_end =
        sizeof(ours::AcpiDirectory) + usize{directory.count} * sizeof(ours::AcpiDirectoryEntry);
    if (entries_end > directory.size) {
        return ustl::err(Status::InvArgs);
    }

    // The tables follow the entries in order, each at a multiple of the
    // alignment, none inside another.
    auto next_free = align_up(entries_end);
    for (auto index = u32{0}; index < directory.count; ++index) {
        auto const entry = entry_at(bytes, index);
        if (entry.reserved != 0 || entry.offset % ours::ACPI_TABLE_ALIGNMENT != 0 ||
            entry.offset < next_free || entry.length < sizeof(acpi::TableHeader) ||
            u64{entry.offset} + entry.length > directory.size) {
            return ustl::err(Status::InvArgs);
        }

        // What the entry says, the table's own header says too.
        acpi::TableHeader header;
        std::memcpy(&header, bytes.data() + entry.offset, sizeof(header));
        if (header.sign.value != entry.signature || header.length != entry.length) {
            return ustl::err(Status::InvArgs);
        }
        if (!sums_to_zero(bytes.subspan(entry.offset, entry.length))) {
            return ustl::err(Status::InvArgs);
        }
        next_free = align_up(usize{entry.offset} + entry.length);
    }

    return ustl::ok(TableDirectory{bytes.first(directory.size), directory.count});
}

auto TableDirectory::find(acpi::Signature const sign) const -> std::span<u8 const> {
    for (auto index = u32{0}; index < m_count; ++index) {
        auto const entry = entry_at(m_bytes, index);
        if (entry.signature == sign.value) {
            return m_bytes.subspan(entry.offset, entry.length);
        }
    }
    return {};
}

} // namespace acpi_route
