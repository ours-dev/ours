/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_ROUTE_TABLE_DIRECTORY_HPP
#define ACPI_ROUTE_TABLE_DIRECTORY_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <span>

#include <acpi/signature.hpp>
#include <ustl/result.hpp>

namespace acpi_route {
/// The ACPI tables the kernel hands user space, read as the layout of
/// <ours/acpi_tables.hpp> says: a directory, then each table's bytes.
///
/// The bytes are the program's to map (ours.boot.Acpi gives the VMO) and come
/// from the firmware by way of the kernel, so nothing in them is trusted until
/// from_bytes() has checked it: that a table lies inside the bytes, is where
/// the directory says and as long as it says, does not overlap another, and
/// adds up to zero, which is ACPI's checksum. A reader of this class never
/// reads outside `bytes`.
class TableDirectory final {
  public:
    /// The most tables a directory may claim. A machine has tens; this keeps a
    /// corrupt count from making the walk long, not a real machine from fitting.
    static constexpr auto MAX_TABLES = u32{256};

    /// Checks `bytes`, which must stay mapped and unchanged for as long as the
    /// result is used. InvArgs for anything that does not hold, with nothing
    /// kept.
    [[nodiscard]]
    static auto from_bytes(std::span<u8 const> bytes) -> ustl::Result<TableDirectory, ours::Status>;

    /// How many tables the directory lists.
    [[nodiscard]]
    auto count() const -> u32 {
        return m_count;
    }

    /// The bytes of the first table whose signature is `sign`, the DSDT being
    /// first of all when there is one, or an empty span when there is none.
    [[nodiscard]]
    auto find(acpi::Signature sign) const -> std::span<u8 const>;

    /// The first table of type `T`, the way acpi::TableSet::find() answers:
    /// nullptr when there is none, or when it is shorter than `T`. `T` is a
    /// packed layout from <acpi/structures/...>, so it may sit at any address.
    template <typename T>
    [[nodiscard]]
    auto find() const -> T const * {
        auto const table = find(T::SIGNATURE);
        if (table.size() < sizeof(T)) {
            return nullptr;
        }
        return reinterpret_cast<T const *>(table.data());
    }

  private:
    constexpr TableDirectory(std::span<u8 const> bytes, u32 count)
        : m_bytes(bytes), m_count(count) {}

    std::span<u8 const> m_bytes;
    u32 m_count;
};

} // namespace acpi_route

#endif // #ifndef ACPI_ROUTE_TABLE_DIRECTORY_HPP
