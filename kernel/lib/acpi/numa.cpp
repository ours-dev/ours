/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <acpi/numa.hpp>

namespace acpi::numa {
auto DistanceMatrix::from_table(slit::Table const &table) -> ktl::Result<DistanceMatrix> {
    auto const total = usize(table.size());
    if (total < sizeof(slit::Table)) {
        return ktl::err(Status::InvalidArgument);
    }

    auto const declared = table.locality_count;
    if (declared > u64(u32(-1))) {
        return ktl::err(Status::OutOfRange);
    }

    // The payload is a count-by-count byte matrix.  Comparing against the
    // quotient rather than forming count * count keeps the check itself from
    // overflowing.
    auto const count = u32(declared);
    auto const available = total - sizeof(slit::Table);
    if (count != 0 && usize(count) > available / usize(count)) {
        return ktl::err(Status::OutOfRange);
    }

    return ktl::ok(DistanceMatrix(&table, count));
}

auto DistanceMatrix::distance(u32 from, u32 to) const -> u8 {
    if (from >= m_count || to >= m_count) {
        return 0;
    }

    return m_table->entry[usize(from) * usize(m_count) + usize(to)];
}

} // namespace acpi::numa
