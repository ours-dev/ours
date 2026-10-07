/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_BOOT_OPTIONS_HPP
#define OURS_PHYS_BOOT_OPTIONS_HPP 1

#include <ours/compiler.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <string_view>

#include <obi/command_line_view.hpp>

namespace ours::phys {
/// The boot options consumed before the kernel is loaded, owned by physload
/// and shared with its loaded modules through PhysLoadHandoff::boot_options.
///
/// The member functions are inline on purpose: physboot reads this object
/// through the handoff pointer from its own image, and an inline body
/// compiles into the reader's image, so the cross-image surface needs no
/// entry in the PHYS_EXPORT export table.  Modules are always rebuilt with
/// physload and the pair is build-id-checked at load time, so private
/// members cost nothing ABI-wise; the field order below is still the handoff
/// layout and must not be reordered.
class BootOptions final {
  public:
    /// Apply the subset of OBI cmdline options consumed before the kernel is
    /// loaded.  Only overwrites the options `cmdline` actually names, which
    /// is what lets physload layer the build-time Config item and the
    /// bootloader's command line: applied in that order, the second wins on
    /// any option both spell.  Unknown tokens are intentionally left for
    /// later boot stages.
    PHYS_EXPORT
    auto apply(obi::CommandLineView const &cmdline) -> Status;

    /// Keep the current debug baseline explicit; `aslr.disabled=0` in the
    /// OBI command line opts into KASLR for every boot entry point.
    [[nodiscard]]
    constexpr auto aslr_disabled() const -> bool {
        return m_aslr_disabled;
    }

    /// The phys module physload loads next (`kernel.phys.next=<name>`);
    /// a BootFS file name inside the kernel package.  The default is the
    /// physboot module — writing `kernel.phys.next=physboot` explicitly is
    /// equivalent to omitting the option.
    [[nodiscard]]
    auto next_module() const -> std::string_view {
        return m_next_module;
    }

    /// The kernel image directory physboot boots (`kernel.name=<dir>`),
    /// resolved inside the same kernel-package BootFS.  File names inside
    /// the directory are fixed vocabulary owned by KernelPackage
    /// (IMAGE_ELF / CODE_PATCHES), not boot parameters.
    [[nodiscard]]
    auto kernel_name() const -> std::string_view {
        return m_kernel_name;
    }

    /// The crash record's region, from `kernel.crashlog=<size>@<address>`.
    /// Both numbers are decimal or 0x-prefixed hex; a size of zero means the
    /// image declares none. Which RAM survives the firmware's run on a warm
    /// reset is a fact about the platform, so the image states it and phys
    /// only reserves it (.agents/tasks/panic-crashlog/DESIGN.md).
    [[nodiscard]]
    constexpr auto crashlog_base() const -> u64 {
        return m_crashlog_base;
    }

    [[nodiscard]]
    constexpr auto crashlog_size() const -> u64 {
        return m_crashlog_size;
    }

  private:
    /// The phys stages' one yes/no option, so it has no word to share; a
    /// second one would start a Flags word in its place.
    bool m_aslr_disabled{true};

    char m_next_module[64]{"physboot"};

    char m_kernel_name[64]{"vmours"};

    u64 m_crashlog_base{0};

    u64 m_crashlog_size{0};
};

/// physload owns the parsed options; modules bind to this object.
PHYS_EXPORT
extern BootOptions BOOT_OPTIONS;

inline auto global_boot_options() -> BootOptions * {
    return &BOOT_OPTIONS;
}

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_BOOT_OPTIONS_HPP
