#include <ours/phys/boot_options.hpp>

#include <string_view>

#include <logz4/filter.hpp>
#include <logz4/level.hpp>

namespace ours::phys {
namespace {
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

    /// A decimal or 0x-prefixed hexadecimal number that fits in a u64.
    auto parse_number(std::string_view text, u64 &out) -> bool {
        auto base = u64{10};
        if (text.starts_with("0x") || text.starts_with("0X")) {
            base = 16;
            text.remove_prefix(2);
        }
        if (text.empty()) {
            return false;
        }
        auto value = u64{0};
        for (auto const character: text) {
            auto digit = u64{0};
            if (character >= '0' && character <= '9') {
                digit = static_cast<u64>(character - '0');
            } else if (base == 16 && character >= 'a' && character <= 'f') {
                digit = static_cast<u64>(character - 'a' + 10);
            } else if (base == 16 && character >= 'A' && character <= 'F') {
                digit = static_cast<u64>(character - 'A' + 10);
            } else {
                return false;
            }
            if (value > (~u64{0} - digit) / base) {
                return false;
            }
            value = value * base + digit;
        }
        out = value;
        return true;
    }

    /// `<size>@<address>`, the shape Linux's memmap= and crashkernel= use.
    auto parse_region(std::string_view text, u64 &size, u64 &base) -> bool {
        auto const at = text.find('@');
        if (at == std::string_view::npos) {
            return false;
        }
        return parse_number(text.substr(0, at), size) && parse_number(text.substr(at + 1), base);
    }
} // namespace

auto BootOptions::apply(obi::CommandLineView const &cmdline) -> Status {
    if (cmdline.contains("aslr.disabled")) {
        m_aslr_disabled = true;
    }
    if (auto const value = cmdline.value("aslr.disabled"); value.is_some()) {
        auto const parsed = obi::CommandLineView::parse_bool(value.value());
        if (parsed.is_none()) {
            return Status::InvArgs;
        }
        m_aslr_disabled = parsed.value();
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
    if (auto const value = cmdline.value("kernel.crashlog"); value.is_some()) {
        if (!parse_region(value.value(), m_crashlog_size, m_crashlog_base)) {
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
