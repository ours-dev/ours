/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_DETAILS_SLIT_HPP
#define ACPI_DETAILS_SLIT_HPP 1

#include <acpi/structures/header.hpp>

namespace acpi::slit {
    struct PACKED Table {
        FORCE_INLINE CXX11_CONSTEXPR
        auto size() const -> usize {
            return header.length;
        }

        TableHeader header;
	    u64 locality_count;
        u8 entry[];

        CXX11_CONSTEXPR
        static Signature const kSignature{"SLIT"};
    };

} // namespace acpi::slit

#endif // #ifndef ACPI_DETAILS_SLIT_HPP