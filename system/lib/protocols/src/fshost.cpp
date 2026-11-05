/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The admin codec's byte level. The request's shape: the wire header, then
// a u32 name size and the name's bytes, with the directory handle riding
// the message's handles.
#include <prot/fshost.hpp>

#include <cstring>

namespace prot::fshost {
namespace {
    constexpr auto NAME_SIZE_OFFSET = usize{16};
    constexpr auto NAME_OFFSET = usize{20};
} // namespace

auto encode_mount(std::span<u8> bytes, std::span<ours::Uandle> handles, MountRequest const &request)
    -> ustl::Result<wire::Message, ours::Status> {
    if (request.name.empty() || request.name.size() > MAX_NAME_BYTES || request.txid == 0 ||
        request.txid > wire::MAX_TXID || handles.size() != 1) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto started =
        wire::start_request(bytes, NAME_OFFSET + request.name.size(), MOUNT, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, NAME_SIZE_OFFSET, static_cast<u32>(request.name.size()));
    std::memcpy(message.data() + NAME_OFFSET, request.name.data(), request.name.size());
    handles[0] = request.handle;
    return ustl::ok(wire::Message{.bytes = message, .handles = handles.first(1)});
}

auto decode_mount(wire::Message message) -> ustl::Result<MountRequest, ours::Status> {
    auto header = wire::decode_request_header(message, MOUNT);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() < NAME_OFFSET || message.handles.size() != 1) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto const size = usize{wire::load<u32>(message.bytes, NAME_SIZE_OFFSET)};
    if (size == 0 || size > MAX_NAME_BYTES || message.bytes.size() != NAME_OFFSET + size) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto const name =
        std::string_view{reinterpret_cast<char const *>(message.bytes.data()) + NAME_OFFSET, size};
    return ustl::ok(MountRequest{.txid = header->txid, .name = name});
}

auto encode_mount_reply(std::span<u8> const bytes, u32 const txid, ours::Status const status)
    -> ustl::Result<wire::Message, ours::Status> {
    return wire::encode_status_reply(bytes, MOUNT, txid, status);
}
} // namespace prot::fshost
