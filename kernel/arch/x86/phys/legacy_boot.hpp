/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LEGACY_BOOT_HPP
#define LEGACY_BOOT_HPP

#include <ours/phys/init.hpp>

#include <multiboot/view.hpp>
#include <ustl/views/span.hpp>
#include <ustl/views/string_view.hpp>

namespace ours::phys {
class Aspace;

class LegacyBoot {
    using Self = LegacyBoot;

  public:
    /// Parse the bounded Multiboot2 handoff that supplies the one OBI
    /// module consumed by physboot.
    [[nodiscard]]
    auto parse_params(usize params) -> Status;

    static auto get() -> LegacyBoot & {
        static LegacyBoot LEGACY_BOOT;
        return LEGACY_BOOT;
    }

    ustl::views::StringView name{};
    ustl::views::StringView cmdline{};
    ustl::views::Span<u8> ramdisk{};
    /// The Multiboot2 info block's own extent, at the address parse_params()
    /// was given.  memory_map below is a *view* into these same bytes, not
    /// a copy -- boot-shim needs this to keep from placing anything on top
    /// of memory memory_map still points into.
    usize mbi_size{0};
    ustl::Option<multiboot::View::MemoryMap> memory_map{};
    usize acpi_version{0}; // New version if 1,
    usize acpi_rsdp{0};
};

} // namespace ours::phys

#endif // #ifndef LEGACY_BOOT_HPP
