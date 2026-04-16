/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_RDST_HPP
#define ACPI_RDST_HPP 1

#include <acpi/structures/header.hpp>

namespace acpi::sdt {
    /// Root system description table pointer.
    struct PACKED Rsdp {
        FORCE_INLINE CXX11_CONSTEXPR
        auto validate_signature() const -> bool {
            return sig1 == kSignature1 || sig2 == kSignature2;
        }

        auto validate() const -> bool;

        Signature sig1;
        Signature sig2;
        u8 checksum;
        u8 oemid[6];
        u8 revision;
        u32 rsdt_addr; // Deprected after the version 2.0

        CXX11_CONSTEXPR
        static Signature const kSignature1{"RSD "};

        CXX11_CONSTEXPR
        static Signature const kSignature2{"PTR "};
    };

    /// Extend system description table pointer.
    struct PACKED RsdpV2 {
        auto validate() const -> bool;

        FORCE_INLINE CXX11_CONSTEXPR
        auto size() const -> usize {
            return length;
        }

        Rsdp v1;
        u32 length;
        u64 xsdt_addr;
        u8 ext_check_sum;
        u8 reserved[3];
    };

    struct PACKED Rsdt {
        FORCE_INLINE CXX11_CONSTEXPR
        auto size() -> usize const {
            return header.length;
        }

        TableHeader header;
        u32 addr32[0];

        CXX11_CONSTEXPR 
        static Signature const kSignature{"RSDT"};
    };
    static_assert(sizeof(Rsdt) == 36);

    struct PACKED Xsdt {
        FORCE_INLINE CXX11_CONSTEXPR
        auto size() -> usize const {
            return header.length;
        }

        CXX11_CONSTEXPR 
        static Signature const kSignature{"XSDT"};

        TableHeader header;
        u64 addr64[0];
    };
    static_assert(sizeof(Xsdt) == 36);

} // namespace acpi::sdt

#endif // #ifndef ACPI_RDST_HPP 