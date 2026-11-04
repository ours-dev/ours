/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROT_DEV_CLIENT_HPP
#define PROT_DEV_CLIENT_HPP 1

#include <prot/dev.hpp>

#include <array>
#include <span>
#include <string_view>

#include <cstring>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The client side of ours.dev, over one connected channel: the codec's
/// publish with the one-at-a-time plumbing a caller would otherwise re-paste
/// -- the txid counter, the two message buffers, the wait and the epitaph
/// check.
///
/// The channel is the caller's to open: the /svc open is component plumbing
/// (fsio::connect on the registry's name, exactly as for any other
/// protocol), and this library stays bytes and calls alone. A Client owns
/// the channel it is given and closes it when it dies.
///
///   auto connection = fsio::connect(prot::dev::REGISTRY_PATH, server_end);
///   prot::dev::Client registry{connection};
///   auto granted = std::array<char, 8>{};
///   registry.publish("block", "000", prot::block::BLOCK_NAME, directory,
///                    granted);
///
/// One thread at a time, as the buffers are shared between the calls: the
/// caller serializes, which one Loop thread makes true.
namespace prot::dev {
class Client final {
  public:
    /// Takes over |channel|, an open connection to a device registry.
    explicit Client(ours::Uandle const channel) : m_channel(channel) {}

    Client(Client const &) = delete;
    auto operator=(Client const &) -> Client & = delete;

    ~Client() {
        if (m_channel != ours::Uandle::Invalid) {
            auto _ = ours::handle_close(m_channel);
        }
    }

    /// Publishes one device: |klass| is the tree section the leaf sits in
    /// ("block"), |name| the proposed node name ("000"), |protocol| the wire
    /// name a client's open of the leaf speaks ("ours.block.Block"), and
    /// |directory| the server end opens of the leaf forward into. The
    /// registry owns the leaf's name: on Ok the granted name -- the
    /// proposal, or the class's lowest free number when it was taken -- is
    /// copied into |granted| and NUL-terminated, BufferTooSmall when the
    /// name and its NUL do not fit. The transferred |directory| cannot be
    /// sent twice, so a failed publish takes a fresh channel to retry.
    [[nodiscard]]
    auto publish(std::string_view const klass, std::string_view const name,
                 std::string_view const protocol, ours::Uandle const directory,
                 std::span<char> granted) -> ours::Status {
        auto const request = encode_publish(m_request, m_request_handles,
                                            {.txid = next_txid(),
                                             .klass = klass,
                                             .name = name,
                                             .protocol = protocol,
                                             .directory = directory});
        if (request.is_err()) {
            return request.unwrap_err();
        }
        auto reply = receive(request.unwrap());
        if (reply.is_err()) {
            return reply.unwrap_err();
        }
        auto const answered = decode_publish_reply(*reply, m_txid);
        if (answered.is_err()) {
            return answered.unwrap_err();
        }
        if (answered->status != ours::Status::Ok) {
            return answered->status;
        }
        if (answered->name.size() + 1 > granted.size()) {
            return ours::Status::BufferTooSmall;
        }
        std::memcpy(granted.data(), answered->name.data(), answered->name.size());
        granted[answered->name.size()] = '\0';
        return ours::Status::Ok;
    }

  private:
    /// Sends |request| and reads the reply, an epitaph's status standing in
    /// for a failure the registry closed the connection with.
    [[nodiscard]]
    auto receive(wire::Message const request) -> ustl::Result<wire::Message, ours::Status> {
        if (auto const written = ours::channel_write(
                m_channel, ours::ChannelWriteOptions::None, request.bytes.data(),
                static_cast<u32>(request.bytes.size()), request.handles.data(),
                static_cast<u32>(request.handles.size()));
            written != ours::Status::Ok) {
            return ustl::err(written);
        }
        auto observed = ours::Signals::None;
        if (auto const waited = ours::object_wait_one(
                m_channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                ours::INSTANT_INFINITE, &observed);
            waited != ours::Status::Ok) {
            return ustl::err(waited);
        }
        if (!(observed & ours::Signals::Readable)) {
            return ustl::err(ours::Status::PeerClosed);
        }
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        if (auto const read = ours::channel_read(
                m_channel, ours::ChannelReadOptions::None, m_reply.data(), m_reply_handles.data(),
                static_cast<u32>(m_reply.size()), static_cast<u32>(m_reply_handles.size()),
                &num_bytes, &num_handles);
            read != ours::Status::Ok) {
            return ustl::err(read);
        }
        for (auto index = u32{0}; index < num_handles; ++index) {
            if (m_reply_handles[index] != ours::Uandle::Invalid) {
                auto _ = ours::handle_close(m_reply_handles[index]);
            }
        }
        auto const message =
            wire::Message{.bytes = std::span{m_reply}.first(num_bytes), .handles = {}};
        // A registry that refused the connection closed it with an epitaph,
        // which is a message like any other until it is asked what it is:
        // its status is the call's failure, not a reply to decode.
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToClient);
        if (header.is_err()) {
            return ustl::err(ours::Status::BadState);
        }
        if (wire::is_epitaph(header.unwrap())) {
            auto const epitaph = wire::decode_epitaph(message);
            return ustl::err(epitaph.is_err() ? ours::Status::BadState : epitaph.unwrap());
        }
        return ustl::ok(message);
    }

    auto next_txid() -> u32 {
        m_txid = m_txid % wire::MAX_TXID + 1;
        return m_txid;
    }

    ours::Uandle m_channel;
    u32 m_txid = 0;
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_request{};
    /// The publish carries one handle, the directory.
    std::array<ours::Uandle, 1> m_request_handles{};
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_reply{};
    std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> m_reply_handles{};
};
} // namespace prot::dev

#endif // #ifndef PROT_DEV_CLIENT_HPP
