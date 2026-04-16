/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LEGACY_BOOT_HPP
#define LEGACY_BOOT_HPP

#include <ours/phys/init.hpp>

#include <ustl/views/span.hpp>
#include <ustl/views/string_view.hpp>

namespace ours::phys {
    class Aspace;

    class LegacyBoot {
        using Self = LegacyBoot;
      public:
        /// Any `LegacyBoot` of a legacy bootloader should provides a 
        /// specific implementation for this this method.
        auto parse_params(usize params) -> void;

        static auto get() -> LegacyBoot & {
            static LegacyBoot LEGACY_BOOT;
            return LEGACY_BOOT;
        }

        ustl::views::StringView name;
        ustl::views::StringView cmdline;
        ustl::views::Span<u8> ramdisk;
        usize acpi_version; // New version if 1,
        usize acpi_rsdp;
    };

    auto init_uart_console() -> void;

} // namespace ours::phys

#endif // #ifndef LEGACY_BOOT_HPP