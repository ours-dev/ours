/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef RPC_RPC_HPP
#define RPC_RPC_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <utility>

#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The call pipeline a two-way protocol client conducts, as an object: one
/// rpc::Channel owns a connected channel, sends its requests, waits, reads
/// the reply, and hands back either the message or the failure -- the
/// epitaph a provider closed the connection with standing in for one.
///
/// This is the plumbing the typed clients pasted per family; it exists so
/// that a client's author writes the typed methods and nothing else. It is
/// also the hand-written prototype of what a protocol generator would emit
/// around each channel.
///
/// Messages travel one at a time per channel: the buffers are members, so
/// the caller serializes, which one Loop thread makes true.
namespace rpc {

/// The channel a typed client conducts over. Takes ownership of the Uandle
/// it is given (closing it when it dies, as the clients' destructors did).
class Channel final {
  public:
    /// Takes over |channel|, an open connection to a provider.
    explicit Channel(ours::Uandle const channel) : m_channel(channel) {}

    Channel(Channel const &) = delete;
    auto operator=(Channel const &) -> Channel & = delete;

    ~Channel() {
        if (m_channel != ours::Uandle::Invalid) {
            auto _ = ours::handle_close(m_channel);
        }
    }

    /// The txid of the request this channel sends next: one past the last,
    /// wrapping inside the protocol's bound.
    auto next_txid() -> u32 {
        m_txid = m_txid % wire::MAX_TXID + 1;
        return m_txid;
    }

    /// Sends |request| and reads the reply. Ok carries the message, whose
    /// bytes view this channel's buffer and are valid until the next call;
    /// Err is the failure -- the provider's epitaph status standing in for
    /// a connection it closed without answering, the wait's or the write's
    /// own otherwise. A reply's stray handles are closed here: no typed
    /// client of a no-handle reply family ever sees one.
    [[nodiscard]]
    auto call(wire::Message const request) -> ustl::Result<wire::Message, ours::Status> {
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
        if (auto const read =
                ours::channel_read(m_channel, ours::ChannelReadOptions::None, m_reply.data(),
                                   m_handles.data(), static_cast<u32>(m_reply.size()),
                                   static_cast<u32>(m_handles.size()), &num_bytes, &num_handles);
            read != ours::Status::Ok) {
            return ustl::err(read);
        }
        for (auto index = u32{0}; index < num_handles; ++index) {
            if (m_handles[index] != ours::Uandle::Invalid) {
                auto _ = ours::handle_close(m_handles[index]);
            }
        }
        auto const message =
            wire::Message{.bytes = std::span{m_reply}.first(num_bytes), .handles = {}};
        // A provider that refused the connection closed it with an epitaph,
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

    /// The reply buffer, for a family whose encoder reads what it answers
    /// from: the same bytes call() fills.
    [[nodiscard]]
    auto reply_buffer() -> std::span<u8> {
        return m_reply;
    }

  private:
    ours::Uandle m_channel;
    u32 m_txid = 0;
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_reply{};
    std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> m_handles{};
};
} // namespace rpc

#endif // #ifndef RPC_RPC_HPP
