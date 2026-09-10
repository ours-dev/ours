/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OBI_COMMAND_LINE_VIEW_HPP
#define OBI_COMMAND_LINE_VIEW_HPP 1

#include <obi/container_view.hpp>

#include <string_view>

#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace obi {
class CommandLineView final {
  public:
    using Result = ustl::Result<CommandLineView, ours::Status>;

    [[nodiscard]]
    static auto from(ItemView const &item) -> Result {
        if (item.type() != ItemType::Config || item.format() != ItemFormat::Raw) {
            return ustl::err(ours::Status::BadType);
        }
        if (item.compression() != Compression::None || item.memory_size() != item.payload_size()) {
            return ustl::err(item.compression() == Compression::None ? ours::Status::InvArgs
                                                                     : ours::Status::NotSupported);
        }
        auto const bytes = item.payload();
        if (bytes.data() == nullptr && bytes.size() != 0) {
            return ustl::err(ours::Status::InvArgs);
        }
        return from_text({reinterpret_cast<char const *>(bytes.data()), bytes.size()});
    }

    /// A command line that did not come out of an OBI item.
    ///
    /// The one caller is a boot-shim forwarding the command line its
    /// legacy protocol handed it (GRUB's Multiboot2 MULTIBOOT_TAG_TYPE_CMDLINE,
    /// say), which has no OBI item to wrap it in: OBI containers are
    /// laid out at build time -- header, item table and string table all
    /// carry baked-in offsets -- so nothing at run time can append an
    /// item to one.  Validation is identical to from() above; only the
    /// provenance of the bytes differs.
    ///
    /// A single trailing NUL is accepted and trimmed (a C string from a
    /// bootloader is the expected shape); an interior NUL is not.
    [[nodiscard]]
    static auto from_text(std::string_view bytes) -> Result {
        if (bytes.data() == nullptr && !bytes.empty()) {
            return ustl::err(ours::Status::InvArgs);
        }
        auto text = bytes;
        if (!text.empty() && text.back() == '\0') {
            text.remove_suffix(1);
        }
        if (text.find('\0') != std::string_view::npos) {
            return ustl::err(ours::Status::InvArgs);
        }
        if (!valid_utf8(text)) {
            return ustl::err(ours::Status::InvArgs);
        }
        if (text.empty()) {
            return ustl::ok(CommandLineView({}));
        }
        return ustl::ok(CommandLineView(text));
    }

    [[nodiscard]]
    auto text() const -> std::string_view {
        return text_;
    }

    [[nodiscard]]
    auto contains(std::string_view option) const -> bool {
        auto const found = find_token(option);
        return found.is_some() && found.value().size() == option.size();
    }

    /// Find a `key=value` token and return only the value portion.
    [[nodiscard]]
    auto value(std::string_view key) const -> ustl::Option<std::string_view> {
        for (usize cursor = 0; cursor < text_.size();) {
            auto const token = next_token(cursor);
            if (token.empty()) {
                break;
            }
            auto const equals = token.find('=');
            if (equals != std::string_view::npos && token.substr(0, equals) == key) {
                return ustl::Option<std::string_view>(token.substr(equals + 1));
            }
        }
        return ustl::NONE;
    }

  private:
    explicit CommandLineView(std::string_view text) : text_(text) {}

    [[nodiscard]]
    static auto is_space(char value) -> bool {
        return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
               value == '\v';
    }

    [[nodiscard]]
    static auto valid_utf8(std::string_view text) -> bool {
        for (usize index = 0; index < text.size();) {
            auto const first = static_cast<unsigned char>(text[index]);
            if (first <= 0x7F) {
                ++index;
                continue;
            }

            auto continuation = [&text, &index](usize count) -> bool {
                if (count > text.size() - index - 1) {
                    return false;
                }
                for (usize offset = 1; offset <= count; ++offset) {
                    if ((static_cast<unsigned char>(text[index + offset]) & 0xC0) != 0x80) {
                        return false;
                    }
                }
                return true;
            };

            if (first >= 0xC2 && first <= 0xDF) {
                if (!continuation(1)) {
                    return false;
                }
                index += 2;
                continue;
            }
            if (first >= 0xE0 && first <= 0xEF) {
                if (!continuation(2)) {
                    return false;
                }
                auto const second = static_cast<unsigned char>(text[index + 1]);
                if ((first == 0xE0 && second < 0xA0) || (first == 0xED && second > 0x9F)) {
                    return false;
                }
                index += 3;
                continue;
            }
            if (first >= 0xF0 && first <= 0xF4) {
                if (!continuation(3)) {
                    return false;
                }
                auto const second = static_cast<unsigned char>(text[index + 1]);
                if ((first == 0xF0 && second < 0x90) || (first == 0xF4 && second > 0x8F)) {
                    return false;
                }
                index += 4;
                continue;
            }
            return false;
        }
        return true;
    }

    [[nodiscard]]
    auto next_token(usize &cursor) const -> std::string_view {
        while (cursor < text_.size() && is_space(text_[cursor])) {
            ++cursor;
        }
        auto const begin = cursor;
        while (cursor < text_.size() && !is_space(text_[cursor])) {
            ++cursor;
        }
        return text_.substr(begin, cursor - begin);
    }

    [[nodiscard]]
    auto find_token(std::string_view option) const -> ustl::Option<std::string_view> {
        for (usize cursor = 0; cursor < text_.size();) {
            auto const token = next_token(cursor);
            if (token.empty()) {
                break;
            }
            if (token == option) {
                return ustl::Option<std::string_view>(token);
            }
        }
        return ustl::NONE;
    }

    std::string_view text_;
};
} // namespace obi

#endif // OBI_COMMAND_LINE_VIEW_HPP
