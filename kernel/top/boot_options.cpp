/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/boot/handoff.hpp>
#include <ours/boot/options.hpp>
#include <ours/panic.hpp>

#include <initializer_list>
#include <limits>

#include <cstring>

#include <logz4/log.hpp>
#include <ustl/option.hpp>

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

    /// `value` as a decimal count, or none.
    ///
    /// Digits only, as many as a u32 holds: no sign, no unit, no base prefix.
    auto parse_digits(std::string_view const value) -> ustl::Option<u32> {
        auto count = u64{0};
        if (value.empty()) {
            return ustl::NONE;
        }
        for (auto const c: value) {
            if (c < '0' || c > '9') {
                return ustl::NONE;
            }
            count = count * 10 + static_cast<u64>(c - '0');
            if (count > std::numeric_limits<u32>::max()) {
                return ustl::NONE;
            }
        }
        return ustl::Option<u32>(static_cast<u32>(count));
    }

    /// Reads `value` as a decimal count into `out`.
    ///
    /// Anything that is not one keeps what `out` had, and says so, like a
    /// boolean outside its grammar.
    auto parse_count(std::string_view const option, std::string_view const value, u32 &out)
        -> void {
        auto const count = parse_digits(value);
        if (count.is_none()) {
            log::error("boot: {}={} is not a count, keeping {}", option, value, out);
            return;
        }
        out = count.value();
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
    apply_flag(cmdline, TIME_ONESHOT_OPTION, Flags::TimeOneshot);
    if (auto const value = cmdline.value(KERNEL_NAME_OPTION); value.is_some()) {
        parse_name(KERNEL_NAME_OPTION, value.value(), m_kernel_name);
    }
    if (auto const value = cmdline.value(LOCKUP_THRESHOLD_OPTION); value.is_some()) {
        parse_count(LOCKUP_THRESHOLD_OPTION, value.value(), m_lockup_threshold);
    }
    struct Count {
        std::string_view option;
        u32 &out;
    };
    for (auto const &[option, out]: {
             Count{OOM_OUT_OF_MEMORY_OPTION, m_oom_out_of_memory_mb},
             Count{OOM_CRITICAL_OPTION, m_oom_critical_mb},
             Count{OOM_WARNING_OPTION, m_oom_warning_mb},
             Count{OOM_DEBOUNCE_OPTION, m_oom_debounce_mb},
             Count{OOM_HYSTERESIS_OPTION, m_oom_hysteresis_seconds},
             Count{PAGE_AGING_OPTION, m_page_aging_seconds},
         }) {
        if (auto const value = cmdline.value(option); value.is_some()) {
            parse_count(option, value.value(), out);
        }
    }

    // The work-stealing knobs: a boolean, then counts.
    for (usize i = 0; i < NUM_STEAL_OPTIONS; ++i) {
        auto const option = STEAL_OPTIONS[i];
        if (0 == i) {
            // A boolean, read as the flags are: the bare key means true.
            if (cmdline.contains(option)) {
                m_steal[i] = 1;
            }
        }
        auto const value = cmdline.value(option);
        if (value.is_none()) {
            continue;
        }
        if (0 != i) {
            // The knob's default is not known here, so a value that is no count
            // leaves the option unset, and the reader keeps its default. The
            // one number that is the mark of "unset" is refused too.
            auto const count = parse_digits(value.value());
            if (count.is_none() || STEAL_UNSET == count.value()) {
                log::error("boot: {}={} is not a count, keeping the default", option,
                           value.value());
                continue;
            }
            m_steal[i] = count.value();
            continue;
        }
        auto const parsed = obi::CommandLineView::parse_bool(value.value());
        if (parsed.is_none()) {
            log::error("boot: {}={} is not a boolean, keeping the default", option, value.value());
            continue;
        }
        m_steal[i] = 0;
        if (parsed.value()) {
            m_steal[i] = 1;
        }
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
