/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/boot/handoff.hpp>
#include <ours/boot/options.hpp>
#include <ours/panic.hpp>

#include <cstring>

#include <logz4/log.hpp>

namespace ours::boot {
namespace {
    BootOptions parsed;

    /// Null until setup_options() has filled `parsed`; see options().
    BootOptions const *installed;

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

    /// Reads a boolean option into `result`: the bare key means true, and
    /// `key=<value>` takes CommandLineView::parse_bool()'s grammar -- the one
    /// phys::BootOptions reads `aslr.disabled` with, so a word means the same
    /// thing to every reader of the one command line. A value outside that
    /// grammar keeps the default, and says so.
    auto parse_flag(obi::CommandLineView const &cmdline, std::string_view const option,
                    bool &result) -> void {
        if (cmdline.contains(option)) {
            result = true;
        }
        if (auto const value = cmdline.value(option); value.is_some()) {
            auto const flag = obi::CommandLineView::parse_bool(value.value());
            if (flag.is_none()) {
                log::error("boot: {}={} is not a boolean, keeping {}", option, value.value(),
                           result);
                return;
            }
            result = flag.value();
        }
    }
} // namespace

auto BootOptions::apply(obi::CommandLineView const &cmdline) -> void {
    if (auto const value = cmdline.value(USERBOOT_OPTION); value.is_some()) {
        parse_name(USERBOOT_OPTION, value.value(), m_userboot);
    }
    parse_flag(cmdline, SHELL_OPTION, m_shell);
    parse_flag(cmdline, DEBUG_READ_OPTION, m_debug_read);
    if (auto const value = cmdline.value(KERNEL_NAME_OPTION); value.is_some()) {
        parse_name(KERNEL_NAME_OPTION, value.value(), m_kernel_name);
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
            parsed.apply(cmdline.value());
        }
    }
    installed = &parsed;
}

auto options() -> BootOptions const & {
    if (installed == nullptr) {
        panic("Boot options have not been parsed");
    }
    return *installed;
}
} // namespace ours::boot
