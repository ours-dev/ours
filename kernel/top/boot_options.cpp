/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/boot/handoff.hpp>
#include <ours/boot/options.hpp>
#include <ours/panic.hpp>

#include <limits>

#include <cstring>

#include <logz4/log.hpp>

namespace ours::boot {
namespace {
    BootOptions PARSED;

    /// Null until setup_options() has filled `PARSED`; see options().
    BootOptions const *INSTALLED;

    /// Copies `value` into one of the fixed-size name buffers.
    ///
    /// A name that does not fit is rejected rather than truncated: a truncated
    /// name would be looked up, not found, and reported as a missing file --
    /// which sends the reader looking in the package for something the kernel
    /// itself mangled.
    template <usize N>
    auto parse_name(std::string_view const option, std::string_view const value, char (&buffer)[N])
        -> void {
        if (value.size() >= N) {
            log::error("boot: {}={} is longer than {} characters, keeping '{}'", option, value,
                       N - 1, std::string_view{buffer});
            return;
        }
        std::memcpy(buffer, value.data(), value.size());
        buffer[value.size()] = '\0';
    }

    /// Reads `value` as a decimal count into `out`.
    ///
    /// Digits only, as many as a u32 holds: no sign, no unit, no base
    /// prefix. Anything else keeps what `out` had, and says so, like a
    /// boolean outside its grammar.
    auto parse_count(std::string_view const option, std::string_view const value, u32 &out)
        -> void {
        auto count = u64{0};
        auto valid = !value.empty();
        for (auto const c: value) {
            if (c < '0' || c > '9') {
                valid = false;
                break;
            }
            count = count * 10 + static_cast<u64>(c - '0');
            if (count > std::numeric_limits<u32>::max()) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            log::error("boot: {}={} is not a count, keeping {}", option, value, out);
            return;
        }
        out = static_cast<u32>(count);
    }
} // namespace

/// The bare key means true, and `key=<value>` takes
/// CommandLineView::parse_bool()'s grammar -- the one phys::BootOptions reads
/// `aslr.disabled` with, so a word means the same thing to every reader of the
/// one command line. A value outside that grammar keeps the default, and says
/// so.
auto BootOptions::apply_flag(obi::CommandLineView const &cmdline, std::string_view const option,
                             Flags const flag) -> void {
    if (cmdline.contains(option)) {
        m_flags |= flag;
    }
    if (auto const value = cmdline.value(option); value.is_some()) {
        auto const parsed = obi::CommandLineView::parse_bool(value.value());
        if (parsed.is_none()) {
            log::error("boot: {}={} is not a boolean, keeping {}", option, value.value(),
                       !!(m_flags & flag));
            return;
        }
        if (parsed.value()) {
            m_flags |= flag;
        } else {
            m_flags &= ~flag;
        }
    }
}

auto BootOptions::apply(obi::CommandLineView const &cmdline) -> void {
    if (auto const value = cmdline.value(USERBOOT_OPTION); value.is_some()) {
        parse_name(USERBOOT_OPTION, value.value(), m_userboot);
    }
    apply_flag(cmdline, SHELL_OPTION, Flags::Shell);
    apply_flag(cmdline, DEBUG_READ_OPTION, Flags::DebugRead);
    apply_flag(cmdline, PANIC_REBOOT_OPTION, Flags::PanicReboot);
    if (auto const value = cmdline.value(KERNEL_NAME_OPTION); value.is_some()) {
        parse_name(KERNEL_NAME_OPTION, value.value(), m_kernel_name);
    }
    if (auto const value = cmdline.value(LOCKUP_THRESHOLD_OPTION); value.is_some()) {
        parse_count(LOCKUP_THRESHOLD_OPTION, value.value(), m_lockup_threshold);
    }
}

auto setup_options() -> void {
    // No package, or a package with no command line, is not an error: it is
    // the common case of an image that asks for nothing, and every option
    // then keeps its default. setup_handoff() has already panicked on a
    // package it could not reconstruct, so is_err() here is not expected --
    // but "the defaults" is the same answer either way.
    auto const package = boot_package();
    if (package.is_ok()) {
        if (auto const cmdline = package.unwrap().command_line(); cmdline.is_some()) {
            PARSED.apply(cmdline.value());
        }
    }
    INSTALLED = &PARSED;
}

auto options() -> BootOptions const & {
    if (INSTALLED == nullptr) {
        panic("Boot options have not been parsed");
    }
    return *INSTALLED;
}

auto try_options() -> BootOptions const * {
    return INSTALLED;
}
} // namespace ours::boot
