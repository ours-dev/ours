/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <fsio/fsio.hpp>
#include <fsio/path.hpp>

#include <algorithm>
#include <array>
#include <limits>

#include <ort/startup.hpp>
#include <ours/process_args.hpp>
#include <ours/signals.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <wire/wire.hpp>

namespace fsio {
namespace {
    using fuchsia_io::Flags;
    using ours::Status;
    using ours::Uandle;

    /// The process's namespace. constinit: nothing runs a user program's
    /// dynamic initialisers (//system/lib/ort, src/start.cpp).
    constinit Namespace installed;

    /// The txid of the next two-way request. One process-wide counter, as a
    /// process has one thread: a reply carries its request's txid back, so a
    /// reply left over from an earlier call, which failed before it read it,
    /// cannot be taken for the answer to this one.
    constinit u32 txid_counter = 1;

    auto next_txid() -> u32 {
        auto const txid = txid_counter;
        if (txid_counter == wire::MAX_TXID) {
            txid_counter = 1;
        } else {
            txid_counter += 1;
        }
        return txid;
    }

    /// One message read off a channel, its bytes and its handles, with room
    /// for the largest a channel carries: a fixed buffer, because a user
    /// program has no heap, and one that cannot be too small, so that a
    /// read never fails with BufferTooSmall and leaves a message behind.
    ///
    /// Owns the handles that came with it until take_handle() claims one,
    /// and closes the rest when it goes. A server that sent handles nobody
    /// asked for, or a malformed reply with handles in it, would otherwise
    /// leave them in this process's table for good.
    class Incoming final {
      public:
        Incoming() = default;
        Incoming(Incoming const &) = delete;
        auto operator=(Incoming const &) -> Incoming & = delete;

        ~Incoming() {
            for (auto index = usize{0}; index < m_num_handles; ++index) {
                if (m_handles[index] != Uandle::Invalid) {
                    // Nothing claimed it, and nothing can be done if the
                    // close fails: the handle is gone either way.
                    auto _ = ours::handle_close(m_handles[index]);
                }
            }
        }

        /// Reads the next message on |channel|, which must be readable.
        [[nodiscard]]
        auto read(Uandle const channel) -> Status {
            return ours::channel_read(channel, ours::ChannelReadOptions::None, m_bytes.data(),
                                      m_handles.data(), static_cast<u32>(m_bytes.size()),
                                      static_cast<u32>(m_handles.size()), &m_num_bytes,
                                      &m_num_handles);
        }

        [[nodiscard]]
        auto message() const -> wire::Message {
            return wire::Message{
                .bytes = std::span{m_bytes}.first(m_num_bytes),
                .handles = std::span{m_handles}.first(m_num_handles),
            };
        }

        /// Takes |handle|, which the message carried, out of the set closed
        /// at the end: the caller owns it from now on.
        auto take_handle(Uandle const handle) -> void {
            for (auto index = usize{0}; index < m_num_handles; ++index) {
                if (m_handles[index] == handle) {
                    m_handles[index] = Uandle::Invalid;
                    return;
                }
            }
        }

      private:
        std::array<u8, wire::MAX_MESSAGE_BYTES> m_bytes{};
        std::array<Uandle, wire::MAX_MESSAGE_HANDLES> m_handles{};
        u32 m_num_bytes = 0;
        u32 m_num_handles = 0;
    };

    /// Writes |request| on |channel| and reads the message that comes back
    /// into |reply|: the answer, an epitaph, or whatever the server wrote
    /// instead, for the caller to decode. The request's handles are consumed
    /// whatever the answer, as channel_write consumes them.
    ///
    /// PeerClosed when the channel closes with nothing left to read. A
    /// server that closes after an epitaph leaves the epitaph readable, so
    /// that it is what the caller finds.
    auto call(Uandle const channel, wire::Message const request, Incoming &reply) -> Status {
        if (auto const status =
                ours::channel_write(channel, ours::ChannelWriteOptions::None, request.bytes.data(),
                                    static_cast<u32>(request.bytes.size()), request.handles.data(),
                                    static_cast<u32>(request.handles.size()));
            status != Status::Ok) {
            return status;
        }
        auto observed = ours::Signals::None;
        if (auto const status =
                ours::object_wait_one(channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                                      ours::INSTANT_INFINITE, &observed);
            status != Status::Ok) {
            return status;
        }
        if (!(observed & ours::Signals::Readable)) {
            return Status::PeerClosed;
        }
        return reply.read(channel);
    }

    /// Why |message| was not the reply its caller decoded it as: the status
    /// of an epitaph, when it is one, and InvArgs, a malformed reply,
    /// otherwise. An Ok epitaph closed the connection with nothing wrong, but
    /// the request still got no answer: PeerClosed.
    auto not_a_reply(wire::Message const message) -> Status {
        auto const epitaph = wire::decode_epitaph(message);
        if (epitaph.is_err()) {
            return Status::InvArgs;
        }
        if (epitaph.unwrap() == Status::Ok) {
            return Status::PeerClosed;
        }
        return epitaph.unwrap();
    }

    /// A request with a small body: every one but Open.
    using SmallRequest = std::array<u8, wire::STATUS_FORM_SIZE>;

    /// The bytes of a Read or ReadDirents Ok reply: at most |limit| of them,
    /// copied into |buffer|. More than asked for is not an answer to the
    /// request: InvArgs.
    auto copy_payload(std::span<u8 const> const payload, usize const limit,
                      std::span<u8> const buffer) -> ustl::Result<usize, Status> {
        if (payload.size() > limit) {
            return ustl::err(Status::InvArgs);
        }
        std::ranges::copy(payload, buffer.begin());
        return ustl::ok(payload.size());
    }
} // namespace

auto installed_namespace() -> Namespace & {
    return installed;
}

auto install() -> Status {
    auto index = u32{0};
    for (auto const name: ort::process_args().names()) {
        // A handle_info's arg is 16 bits, so no handle names a later name.
        // A 4 KiB message holds fewer strings than that anyway.
        if (index > std::numeric_limits<u16>::max()) {
            break;
        }
        auto const directory =
            ort::take_startup_handle(ours::HandleType::NsDir, static_cast<u16>(index));
        index += 1;
        if (directory == Uandle::Invalid) {
            continue;
        }
        if (auto const status = installed.bind(name, directory); status != Status::Ok) {
            // The namespace did not take it, and nothing else will.
            auto _ = ours::handle_close(directory);
            return status;
        }
    }
    return Status::Ok;
}

auto open(std::string_view const path, Flags const flags, Uandle const object) -> Status {
    // Consumed on every path, as the write below consumes it: a caller never
    // has to work out whether it still owns |object|.
    auto const refuse = [object](Status const status) {
        auto _ = ours::handle_close(object);
        return status;
    };

    PathBuffer clean;
    if (auto const status = clean_path(path, clean); status != Status::Ok) {
        return refuse(status);
    }
    auto const resolved = installed.resolve(clean);
    if (resolved.is_err()) {
        return refuse(resolved.unwrap_err());
    }

    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, 1> handles{};
    auto const encoded = fuchsia_io::encode_open(bytes, handles,
                                                 fuchsia_io::OpenRequest{
                                                     .flags = flags,
                                                     .path = resolved->rest,
                                                     .object = object,
                                                 });
    if (encoded.is_err()) {
        return refuse(encoded.unwrap_err());
    }
    auto const message = encoded.unwrap();
    return ours::channel_write(resolved->directory, ours::ChannelWriteOptions::None,
                               message.bytes.data(), static_cast<u32>(message.bytes.size()),
                               message.handles.data(), static_cast<u32>(message.handles.size()));
}

auto open(std::string_view const path, Flags const flags) -> ustl::Result<Uandle, Status> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const status = open(path, flags, server); status != Status::Ok) {
        auto _ = ours::handle_close(client);
        return ustl::err(status);
    }
    return ustl::ok(client);
}

auto connect(std::string_view const path, Uandle const server) -> Status {
    return open(path, Flags::ProtocolService, server);
}

auto read(Uandle const file, std::span<u8> const buffer) -> ustl::Result<usize, Status> {
    auto const count = std::min(buffer.size(), fuchsia_io::MAX_TRANSFER_SIZE);
    auto const txid = next_txid();
    SmallRequest bytes{};
    auto const request =
        fuchsia_io::encode_read(bytes, fuchsia_io::ReadRequest{.txid = txid, .count = count});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    Incoming reply;
    if (auto const status = call(file, request.unwrap(), reply); status != Status::Ok) {
        return ustl::err(status);
    }
    auto const decoded = fuchsia_io::decode_read_reply(reply.message(), txid);
    if (decoded.is_err()) {
        return ustl::err(not_a_reply(reply.message()));
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    return copy_payload(decoded->data, count, buffer);
}

auto read_dirents(Uandle const directory, std::span<u8> const buffer)
    -> ustl::Result<usize, Status> {
    auto const max_bytes = std::min(buffer.size(), fuchsia_io::MAX_BUF);
    auto const txid = next_txid();
    SmallRequest bytes{};
    auto const request = fuchsia_io::encode_read_dirents(
        bytes, fuchsia_io::ReadDirentsRequest{.txid = txid, .max_bytes = max_bytes});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    Incoming reply;
    if (auto const status = call(directory, request.unwrap(), reply); status != Status::Ok) {
        return ustl::err(status);
    }
    auto const decoded = fuchsia_io::decode_read_dirents_reply(reply.message(), txid);
    if (decoded.is_err()) {
        return ustl::err(not_a_reply(reply.message()));
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    return copy_payload(decoded->dirents, max_bytes, buffer);
}

auto get_backing_memory(Uandle const file, fuchsia_io::VmoFlags const flags)
    -> ustl::Result<Uandle, Status> {
    auto const txid = next_txid();
    SmallRequest bytes{};
    auto const request = fuchsia_io::encode_get_backing_memory(
        bytes, fuchsia_io::GetBackingMemoryRequest{.txid = txid, .flags = flags});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    Incoming reply;
    if (auto const status = call(file, request.unwrap(), reply); status != Status::Ok) {
        return ustl::err(status);
    }
    auto const decoded = fuchsia_io::decode_get_backing_memory_reply(reply.message(), txid);
    if (decoded.is_err()) {
        return ustl::err(not_a_reply(reply.message()));
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    reply.take_handle(decoded->vmo);
    return ustl::ok(decoded->vmo);
}

auto close(Uandle const node) -> Status {
    auto const answer = [node]() -> Status {
        auto const txid = next_txid();
        SmallRequest bytes{};
        auto const request =
            fuchsia_io::encode_close(bytes, fuchsia_io::CloseRequest{.txid = txid});
        if (request.is_err()) {
            return request.unwrap_err();
        }
        Incoming reply;
        if (auto const status = call(node, request.unwrap(), reply); status != Status::Ok) {
            return status;
        }
        auto const decoded = fuchsia_io::decode_close_reply(reply.message(), txid);
        if (decoded.is_err()) {
            return not_a_reply(reply.message());
        }
        return decoded->status;
    }();
    // The connection is over whatever the server said, and the handle with
    // it; a failure to close a handle the caller gave up has no one to tell.
    auto _ = ours::handle_close(node);
    return answer;
}
} // namespace fsio
