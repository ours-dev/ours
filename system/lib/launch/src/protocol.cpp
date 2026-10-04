/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/protocol.hpp>

#include <cstring>

#include <ours/syscall.hpp>

namespace launch::protocol {
namespace {
    using ours::Status;

    /// Where the processargs part starts, after a name of |name_size| bytes:
    /// the header and the name, rounded up to 4 bytes, the alignment the
    /// processargs header and its handle_info array need.
    constexpr auto process_args_offset(u32 const name_size) -> usize {
        return (sizeof(Header) + name_size + 3) & ~usize{3};
    }

    auto names_vma_root(std::span<u32 const> const handle_info) -> bool {
        for (auto const info: handle_info) {
            if (ours::handle_info_type(info) == ours::HandleType::VmaRoot) {
                return true;
            }
        }
        return false;
    }
} // namespace

auto encode_launch(std::span<u8> const out, std::string_view const name,
                   std::span<u32 const> const handle_info,
                   std::span<std::string_view const> const args,
                   std::span<std::string_view const> const environ,
                   std::span<std::string_view const> const names, u32 *const length) -> Status {
    if (name.empty() || name.size() > MAX_PROGRAM_NAME || names_vma_root(handle_info)) {
        return Status::InvArgs;
    }
    // The job travels in front of the program's handles.
    if (handle_info.size() > ours::CHANNEL_MAX_MSG_HANDLES - 1 || args.size() > MAX_STRINGS ||
        environ.size() > MAX_STRINGS || names.size() > MAX_STRINGS) {
        return Status::OutOfRange;
    }

    auto const name_size = static_cast<u32>(name.size());
    auto const offset = process_args_offset(name_size);
    auto const room =
        out.size() < ours::CHANNEL_MAX_MSG_BYTES ? out.size() : usize{ours::CHANNEL_MAX_MSG_BYTES};
    if (offset > room) {
        return Status::OutOfRange;
    }

    u32 process_args_size = 0;
    if (auto const status =
            ours::encode_process_args(out.subspan(offset, room - offset), handle_info, args,
                                      environ, names, &process_args_size);
        status != Status::Ok) {
        return status;
    }

    auto const header = Header{.ordinal = Ordinal::Launch, .name_size = name_size};
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), name.data(), name.size());
    std::memset(out.data() + sizeof(header) + name.size(), 0,
                offset - sizeof(header) - name.size());
    *length = static_cast<u32>(offset) + process_args_size;
    return Status::Ok;
}

auto decode_launch(std::span<u8 const> const message, u32 const num_handles, Launch *const out)
    -> Status {
    Header header{};
    if (message.size() < sizeof(header)) {
        return Status::InvArgs;
    }
    std::memcpy(&header, message.data(), sizeof(header));
    if (header.ordinal != Ordinal::Launch || header.name_size == 0 ||
        header.name_size > MAX_PROGRAM_NAME) {
        return Status::InvArgs;
    }
    auto const offset = process_args_offset(header.name_size);
    if (offset > message.size() || num_handles == 0) {
        return Status::InvArgs;
    }

    auto const process_args = message.subspan(offset);
    ort::ProcessArgs parsed;
    if (auto const status = ort::ProcessArgs::parse(process_args, num_handles - 1, &parsed);
        status != Status::Ok) {
        return Status::InvArgs;
    }
    if (parsed.args().size() > MAX_STRINGS || parsed.environ().size() > MAX_STRINGS ||
        parsed.names().size() > MAX_STRINGS) {
        return Status::InvArgs;
    }
    for (u32 index = 0; index < parsed.num_handles(); ++index) {
        if (ours::handle_info_type(parsed.handle_info(index)) == ours::HandleType::VmaRoot) {
            return Status::InvArgs;
        }
    }

    out->name = {reinterpret_cast<char const *>(message.data() + sizeof(header)), header.name_size};
    out->process_args = process_args;
    out->parsed = parsed;
    return Status::Ok;
}

auto ordinal_of(std::span<u8 const> const message) -> ustl::Option<Ordinal> {
    if (message.size() < sizeof(Header)) {
        return ustl::NONE;
    }
    Header header{};
    std::memcpy(&header, message.data(), sizeof(header));
    return ustl::some(header.ordinal);
}
} // namespace launch::protocol
