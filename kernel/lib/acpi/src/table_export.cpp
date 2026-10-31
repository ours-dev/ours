/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <acpi/table_export.hpp>

#include <cstring>

#include <ours/acpi_tables.hpp>

namespace acpi {
namespace {
    constexpr auto DSDT_SIGNATURE = Signature::from_chars("DSDT");

    constexpr auto align_up(usize const value) -> usize {
        constexpr auto MASK = ours::ACPI_TABLE_ALIGNMENT - 1;
        return (value + MASK) & ~MASK;
    }

    /// Calls `visit` on the header of every table the output holds, in the
    /// order it holds them, and answers how many tables of the root table were
    /// left out because they could not be mapped or their checksum did not
    /// hold.  Both passes of the export walk the same tables through this, so
    /// the plan and the writing cannot disagree about which they are.
    template <typename Visit>
    auto for_each_exported(TableSet &tables, Visit &&visit) -> usize {
        if (auto const *dsdt = tables.dsdt()) {
            visit(*dsdt);
        }

        auto skipped = usize(0);
        for (auto i = usize(0); i < tables.num_tables(); ++i) {
            auto const maybe_header = tables.header_at(i);
            if (!maybe_header) {
                ++skipped;
                continue;
            }

            auto const *header = maybe_header.unwrap();
            // The DSDT is not meant to be in the root table.  If a firmware
            // lists one anyway, the FADT's is the one that is exported.
            if (header->sign == DSDT_SIGNATURE) {
                continue;
            }
            if (!validate_checksum(header, header->length)) {
                ++skipped;
                continue;
            }
            visit(*header);
        }
        return skipped;
    }

    constexpr auto directory_bytes(usize const count) -> usize {
        return align_up(sizeof(ours::AcpiDirectory) + count * sizeof(ours::AcpiDirectoryEntry));
    }
} // namespace

auto plan_export(TableSet &tables) -> ExportedLayout {
    auto layout = ExportedLayout{};
    auto bytes = usize(0);
    layout.skipped = for_each_exported(tables, [&](TableHeader const &header) {
        // The firmware's memory is read once per table: a length read twice
        // could be two lengths.
        auto const length = usize(header.length);
        layout.has_dsdt = layout.has_dsdt || header.sign == DSDT_SIGNATURE;
        ++layout.count;
        bytes += align_up(length);
    });
    layout.size = directory_bytes(layout.count) + bytes;
    return layout;
}

auto export_tables(TableSet &tables, std::span<u8> const out) -> ktl::Result<usize> {
    auto const layout = plan_export(tables);
    if (out.size() < layout.size) {
        return ktl::err(Status::BufferTooSmall);
    }
    // The offsets are 32 bit: refuse what would not fit rather than wrap.
    if (u64(layout.size) > u64(~u32{0})) {
        return ktl::err(Status::OutOfRange);
    }

    // The whole of what is used, so that the padding is zero and nothing of
    // what the buffer held before reaches user space.
    std::memset(out.data(), 0, layout.size);

    auto const directory = ours::AcpiDirectory{
        .magic = ours::ACPI_DIRECTORY_MAGIC,
        .version = ours::ACPI_DIRECTORY_VERSION,
        .count = u32(layout.count),
        .size = u32(layout.size),
    };
    std::memcpy(out.data(), &directory, sizeof(directory));

    auto const tables_begin = directory_bytes(layout.count);
    auto next_entry = sizeof(ours::AcpiDirectory);
    auto next_table = tables_begin;
    auto written = usize(0);
    auto overran = false;
    for_each_exported(tables, [&](TableHeader const &header) {
        // The tables are firmware memory that nothing writes, so the second
        // walk sees what the first did.  Checked all the same, because what
        // is written next lands in a buffer of the caller's: a table that did
        // not fit the plan is not written past the end, and the walk is held
        // against the plan once it is done.  The length is read once, and the
        // bound, the entry, the copy and the stride all use that read.
        auto const length = usize(header.length);
        if (written == layout.count || next_table + length > layout.size) {
            overran = true;
            return;
        }
        auto const entry = ours::AcpiDirectoryEntry{
            .signature = header.sign.value,
            .offset = u32(next_table),
            .length = u32(length),
            .reserved = 0,
        };
        std::memcpy(out.data() + next_entry, &entry, sizeof(entry));
        std::memcpy(out.data() + next_table, static_cast<void const *>(&header), length);
        next_entry += sizeof(entry);
        next_table += align_up(length);
        ++written;
    });

    // The directory says how many tables there are and how long it all is.  If
    // the second walk found other tables than the first, it would say what
    // was not written, so it is an error and not a directory.
    if (overran || written != layout.count || next_table != layout.size) {
        return ktl::err(Status::BadState);
    }
    return ktl::ok(layout.size);
}

} // namespace acpi
