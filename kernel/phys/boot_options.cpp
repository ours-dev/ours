#include <logz4/filter.hpp>
#include <logz4/level.hpp>
#include <ours/phys/boot_options.hpp>

#include <string_view>

namespace ours::phys {
namespace {
    auto parse_bool(std::string_view value, bool &result) -> bool {
        if (value == "1" || value == "true" || value == "yes") {
            result = true;
            return true;
        }
        if (value == "0" || value == "false" || value == "no") {
            result = false;
            return true;
        }
        return false;
    }

    /// Copy a value into one of the fixed-size BootOptions name buffers.
    /// A value that does not fit is a malformed command line, not a
    /// truncatable one.
    auto parse_name(std::string_view value, char *buffer, usize capacity) -> bool {
        if (value.empty() || value.size() >= capacity) {
            return false;
        }
        auto *cursor = buffer;
        for (auto const character: value) {
            *cursor++ = character;
        }
        *cursor = '\0';
        return true;
    }
} // namespace

auto BootOptions::apply(obi::CommandLineView const &cmdline) -> Status {
    if (cmdline.contains("aslr.disabled")) {
        m_aslr_disabled = true;
    }
    if (auto const value = cmdline.value("aslr.disabled"); value.is_some()) {
        if (!parse_bool(value.value(), m_aslr_disabled)) {
            return Status::InvArgs;
        }
    }
    if (auto const value = cmdline.value("kernel.phys.next"); value.is_some()) {
        if (!parse_name(value.value(), m_next_module, sizeof(m_next_module))) {
            return Status::InvArgs;
        }
    }
    if (auto const value = cmdline.value("kernel.name"); value.is_some()) {
        if (!parse_name(value.value(), m_kernel_name, sizeof(m_kernel_name))) {
            return Status::InvArgs;
        }
    }
    if (auto const value = cmdline.value("log.level"); value.is_some()) {
        auto level = logz4::Level::Info;
        if (!logz4::parse_level(value.value(), level)) {
            return Status::InvArgs;
        }
        logz4::set_min_level(level);
    }
    return Status::Ok;
}
} // namespace ours::phys
