/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/client.hpp>
#include <launch/protocol.hpp>

#include <cstring>

#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace launch {
namespace {
    using ours::Status;
    using ours::Uandle;

    auto close_all(std::span<Uandle const> const handles) -> void {
        for (auto const handle: handles) {
            auto _ = ours::handle_close(handle);
        }
    }
} // namespace

auto Client::launch(Uandle const job, std::string_view const name,
                    std::span<std::string_view const> const args,
                    std::span<std::string_view const> const environ,
                    std::span<Uandle const> const handles,
                    std::span<u32 const> const handle_info) const -> ustl::Result<Uandle, Status> {
    if (handles.size() != handle_info.size()) {
        close_all(handles);
        return ustl::err(Status::InvArgs);
    }

    // On the stack: one channel message, which the user stack has room for.
    u8 message[ours::CHANNEL_MAX_MSG_BYTES];
    u32 length = 0;
    if (auto const status =
            protocol::encode_launch(message, name, handle_info, args, environ, {}, &length);
        status != Status::Ok) {
        close_all(handles);
        return ustl::err(status);
    }

    // The job first, a duplicate, then the program's handles.
    Uandle all[ours::CHANNEL_MAX_MSG_HANDLES];
    if (auto const status = ours::handle_duplicate(job, ours::Rights::SameRights, &all[0]);
        status != Status::Ok) {
        close_all(handles);
        return ustl::err(status);
    }
    u32 count = 1;
    for (auto const handle: handles) {
        all[count++] = handle;
    }
    // A count within the limit and an array that can be read: the write
    // consumes every handle whatever it answers.
    if (auto const status = ours::channel_write(m_channel, ours::ChannelWriteOptions::None, message,
                                                length, all, count);
        status != Status::Ok) {
        return ustl::err(status);
    }

    auto observed = ours::Signals::None;
    if (auto const status =
            ours::object_wait_one(m_channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                                  ours::INSTANT_INFINITE, &observed);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (!(observed & ours::Signals::Readable)) {
        return ustl::err(Status::PeerClosed);
    }

    protocol::Reply reply{};
    auto process = Uandle::Invalid;
    u32 actual_bytes = 0;
    u32 actual_handles = 0;
    if (auto const status =
            ours::channel_read(m_channel, ours::ChannelReadOptions::None, &reply, &process,
                               sizeof(reply), 1, &actual_bytes, &actual_handles);
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto const well_formed =
        actual_bytes == sizeof(reply) && reply.ordinal == protocol::Ordinal::Launch &&
        (reply.status == Status::Ok ? actual_handles == 1 : actual_handles == 0);
    if (!well_formed) {
        if (actual_handles != 0) {
            auto _ = ours::handle_close(process);
        }
        return ustl::err(Status::InvArgs);
    }
    if (reply.status != Status::Ok) {
        return ustl::err(reply.status);
    }
    return ustl::ok(process);
}

auto Client::connect() const -> ustl::Result<Uandle, Status> {
    Uandle mine = Uandle::Invalid;
    Uandle theirs = Uandle::Invalid;
    if (auto const status = ours::channel_create(ours::ChannelCreateOptions::None, &mine, &theirs);
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto const header = protocol::Header{.ordinal = protocol::Ordinal::Connect, .name_size = 0};
    if (auto const status = ours::channel_write(m_channel, ours::ChannelWriteOptions::None, &header,
                                                sizeof(header), &theirs, 1);
        status != Status::Ok) {
        auto _ = ours::handle_close(mine);
        return ustl::err(status);
    }
    return ustl::ok(mine);
}
} // namespace launch
