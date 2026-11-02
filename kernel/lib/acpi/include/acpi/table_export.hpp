/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_TABLE_EXPORT_HPP
#define ACPI_TABLE_EXPORT_HPP 1

#include <acpi/table_set.hpp>

#include <span>

namespace acpi {
/// Lays the machine's tables out for user space, in the format of
/// <ours/acpi_tables.hpp>: a directory, then each table's bytes, copied as the
/// firmware left them.
///
/// Which tables: the DSDT first, then the tables the root table lists, in its
/// order.  One that cannot be mapped, or whose checksum does not hold, is left
/// out, and `skipped` counts them.
///
/// The format has no pointers, so the output can be moved anywhere, and the
/// whole of it is written: the padding between tables is zero.
struct ExportedLayout {
    /// The tables that will be in the output.
    usize count{0};
    /// The bytes the directory and the tables take.
    usize size{0};
    /// The tables of the root table that were left out: they could not be
    /// mapped, or their checksum did not hold.  A DSDT the root table lists is
    /// not counted: it is ignored on purpose, the FADT's being the DSDT.
    usize skipped{0};
    /// Whether the DSDT, which the FADT points to, is among the tables.
    bool has_dsdt{false};
};

/// What export_tables() would write for `tables`.
[[nodiscard]]
auto plan_export(TableSet &tables) -> ExportedLayout;

/// Writes the directory and the tables into `out`, which must hold at least
/// `plan_export(tables).size` bytes, and answers how many bytes were used.
/// BufferTooSmall if `out` is shorter, with nothing written.  BadState if the
/// tables are not the ones the plan counted when they are written (firmware
/// memory is not meant to change, but the directory says how many tables
/// there are, and it must not say it of tables that were not written); `out`
/// is then not a directory and is to be dropped.
[[nodiscard]]
auto export_tables(TableSet &tables, std::span<u8> out) -> ktl::Result<usize>;

} // namespace acpi

#endif // #ifndef ACPI_TABLE_EXPORT_HPP
