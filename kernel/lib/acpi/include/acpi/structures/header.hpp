/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_DETAILS_HEADER_HPP
#define ACPI_DETAILS_HEADER_HPP 1

#include <acpi/types.hpp>

#include <ours/compiler.hpp>

#include <ustl/traits/is_same.hpp>

namespace acpi {
    union PACKED Signature {
        // Create an AcpiSignature from a C-style string.
        CXX11_CONSTEXPR
        Signature() = default;

        CXX11_CONSTEXPR
        ~Signature() = default;

        CXX11_CONSTEXPR
        explicit Signature(char const name[4]) {
            this->name[0] = name[0];
            this->name[1] = name[1];
            this->name[2] = name[2];
            this->name[3] = name[3];
        }

        // Operators.
        CXX11_CONSTEXPR
        friend bool operator==(Signature const &x, Signature const &y) {
            return x.value == y.value;
        }

        CXX11_CONSTEXPR
        friend bool operator!=(Signature const &x, Signature const &y) {
            return x.value != y.value;
        }

        u32 value;
        char name[4];
    };
    static_assert(sizeof(Signature) == 4);

    struct PACKED EntryHeader {
        auto size() const -> u8 { return length; }

        u8 type;
        u8 length;
    };

    /// System description table header.
    struct PACKED TableHeader {
        auto size() const -> u8 { return length; }

        Signature sign;
        u32 length;
        u8 revision;
        u8 checksum;
        char oem_id[6];
        char oem_tableid[8];
        u32 oem_revision;
        u32 creator_id;
        u32 creator_revision;
    };
    static_assert(sizeof(TableHeader) == 36);

    template <typename Dest, typename Src>
    auto downcast(Src const *src) -> Dest const * {
        static_assert(offsetof(Dest, header) == 0,
                      "Expected field |header| to be first field in struct.");
        static_assert(ustl::traits::IsSameV<decltype(Dest::header), Src>,
                      "Expected |Dest::header| type to match |Src|.");
        if (src->size() < sizeof(Dest)) {
            return nullptr;
        }
        return reinterpret_cast<Dest const *>(src);
    }

    struct PACKED GenericAddress {
        u8 address_space_id;    // 0 - system memory, 1 - system I/O
        u8 register_bit_width;
        u8 register_bit_offset;
        u8 reserved;
        u64 address;
    };

    CXX11_CONSTEXPR
    static auto const kAspaceSysMemId = 0;

    CXX11_CONSTEXPR
    static auto const kAspaceSysIoId = 1;

} // namespace acpi

#endif // #ifndef ACPI_DETAILS_HEADER_HPP