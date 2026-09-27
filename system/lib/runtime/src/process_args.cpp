/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <runtime/process_args.hpp>

#include <cstring>

namespace runtime {
using ours::Status;

namespace {
    /// Checks the |num| strings of a section at |off| in a message of |bytes|
    /// bytes, and answers where the first one starts.
    ///
    /// An empty section's offset is not looked at: the encoder writes one,
    /// but a message that names no strings has nothing there to check. A
    /// non-empty one must start after the header and before the end, and
    /// have at least a byte -- the NUL -- per string left before the end;
    /// that bound is written as a subtraction from |bytes| so that no sum
    /// can wrap. Then each string must actually end: a scan for its NUL that
    /// reaches the end of the message fails the whole message, where
    /// Zircon's newer decoder would return the strings it had found so far.
    [[nodiscard]]
    auto check_strings(u8 const *const message, usize const bytes, u32 const off, u32 const num,
                       char const **const first) -> Status {
        if (num == 0) {
            *first = nullptr;
            return Status::Ok;
        }
        if (off < sizeof(ours::ProcArgs) || off >= bytes || num > bytes - off) {
            return Status::InvArgs;
        }
        auto const *const start = reinterpret_cast<char const *>(message) + off;
        auto const *const limit = reinterpret_cast<char const *>(message) + bytes;
        auto const *at = start;
        for (u32 i = 0; i < num; ++i) {
            auto const *const nul =
                static_cast<char const *>(std::memchr(at, 0, static_cast<usize>(limit - at)));
            if (nul == nullptr) {
                return Status::InvArgs;
            }
            at = nul + 1;
        }
        *first = start;
        return Status::Ok;
    }
} // namespace

auto ProcessArgs::parse(std::span<u8 const> const message, u32 const num_handles,
                        ProcessArgs *const out) -> Status {
    auto const bytes = message.size();
    if (bytes < sizeof(ours::ProcArgs)) {
        return Status::InvArgs;
    }
    // Copied out rather than read through a cast, so the message buffer
    // needs no particular alignment.
    ours::ProcArgs header;
    std::memcpy(&header, message.data(), sizeof(header));
    if (header.protocol != ours::PROCARGS_PROTOCOL || header.version != ours::PROCARGS_VERSION) {
        return Status::InvArgs;
    }

    // Zircon's musl processargs_read() bounds the handle_info array exactly
    // this way. The newer ld::ProcessargsBuffer::Valid() subtracts the
    // offset in words from the size in bytes, and so accepts, say, a 40-byte
    // message that claims five handle_infos at offset 36.
    auto const info_off = header.handle_info_off;
    if (info_off < sizeof(ours::ProcArgs) || info_off % sizeof(u32) != 0 || info_off > bytes ||
        (bytes - info_off) / sizeof(u32) < num_handles) {
        return Status::InvArgs;
    }

    ProcessArgs view;
    view.m_message = message.data();
    view.m_num_handles = num_handles;
    view.m_handle_info_off = info_off;
    for (u32 i = 0; i < num_handles; ++i) {
        if ((view.handle_info(i) & ours::HANDLE_INFO_RESERVED) != 0) {
            return Status::InvArgs;
        }
    }

    struct Section {
        u32 off;
        u32 num;
        Strings *strings;
    };
    Section const sections[] = {
        {header.args_off, header.args_num, &view.m_args},
        {header.environ_off, header.environ_num, &view.m_environ},
        {header.names_off, header.names_num, &view.m_names},
    };
    for (auto const &section: sections) {
        char const *first = nullptr;
        if (auto const status =
                check_strings(message.data(), bytes, section.off, section.num, &first);
            status != Status::Ok) {
            return status;
        }
        *section.strings = Strings(first, section.num);
    }

    *out = view;
    return Status::Ok;
}

auto ProcessArgs::option(std::string_view const name) const -> std::string_view {
    std::string_view value;
    for (auto const entry: m_environ) {
        if (entry.size() > name.size() && entry.starts_with(name) && entry[name.size()] == '=') {
            value = entry.substr(name.size() + 1);
        }
    }
    return value;
}

auto ProcessArgs::handle_info(u32 const index) const -> u32 {
    u32 info;
    std::memcpy(&info, m_message + m_handle_info_off + usize{index} * sizeof(u32), sizeof(info));
    return info;
}
} // namespace runtime
