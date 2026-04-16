/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_DETAILS_DBG2T_HPP
#define ACPI_DETAILS_DBG2T_HPP 1

#include <acpi/structures/header.hpp>

namespace acpi {
    // DBG2 table
    struct PACKED Dbg2Table {
        TableHeader header;
        u32 offset;
        u32 num_entries;

        FORCE_INLINE CXX11_CONSTEXPR
        auto size() const -> usize { 
            return header.length; 
        }

        CXX11_CONSTEXPR
        static Signature kSignature{"DBG2"};
    };
    static_assert(sizeof(Dbg2Table) == 44);

    enum class Dbg2Type: u16 {
        SerialPort = 0x8000,
        T1394Port = 0x8001,
        UsbPort = 0x8002,
        NetPort = 0x8003,
    };

    enum class Dbg2SubType: u16 {
        T16550Compatible = 0x0000,
        T16550Subset = 0x0001,
        T1394Standard = 0x0000,
        UsbXhci = 0x0000,
        UsbEhci = 0x0001,
    };

    struct PACKED Dbg2Device {
        u8 revision;
        u16 length;
        u8 register_count;
        u16 namepath_length;
        u16 namepath_offset;
        u16 oem_data_length;
        u16 oem_data_offset;
        Dbg2Type port_type;
        Dbg2SubType port_subtype;
        u16 reserved;
        u16 base_address_offset;
        u16 address_size_offset;

        FORCE_INLINE CXX11_CONSTEXPR
        auto size() const -> usize { 
            return length; 
        }
    };
    static_assert(sizeof(Dbg2Device) == 22);

} // namespace acpi

#endif // #ifndef ACPI_DETAILS_DBG2T_HPP