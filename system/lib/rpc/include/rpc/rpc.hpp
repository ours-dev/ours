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

/// The call pipeline a two-way protocol client exchanges over, as an object: one
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

/// The pipeline over |channel|, which the caller owns: send, wait, read,
/// and return the message -- handles intact, any epitaph untranslated
/// (messages, not verdicts). The reply lands in |reply_bytes| and
/// |reply_handles|, the caller's: a view of the pipeline's own stack would
/// die at the return, which is the one shape this must not have. A write
/// that finds the connection closed is not the end -- an epitaph queued
/// before the peer died is still there to read, so PeerClosed falls
/// through to the read, and what comes back names itself.
[[nodiscard]]
inline auto exchange(ours::Uandle const channel, wire::Message const &request,
                     std::span<u8> const reply_bytes, std::span<ours::Uandle> const reply_handles)
    -> ustl::Result<wire::Message, ours::Status> {
    if (auto const written =
            ours::channel_write(channel, ours::ChannelWriteOptions::None, request.bytes.data(),
                                static_cast<u32>(request.bytes.size()), request.handles.data(),
                                static_cast<u32>(request.handles.size()));
        written != ours::Status::Ok && written != ours::Status::PeerClosed) {
        return ustl::err(written);
    }
    auto observed = ours::Signals::None;
    if (auto const waited =
            ours::object_wait_one(channel, ours::Signals::Readable | ours::Signals::PeerClosed,
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
            ours::channel_read(channel, ours::ChannelReadOptions::None, reply_bytes.data(),
                               reply_handles.data(), static_cast<u32>(reply_bytes.size()),
                               static_cast<u32>(reply_handles.size()), &num_bytes, &num_handles);
        read != ours::Status::Ok) {
        return ustl::err(read);
    }
    return ustl::ok(wire::Message{.bytes = reply_bytes.first(num_bytes),
                                  .handles = reply_handles.first(num_handles)});
}

/// The channel a typed client exchanges over. Takes ownership of the Uandle
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
    /// bytes view this call's buffer and are valid until the next call; Err
    /// is the failure -- the provider's epitaph status standing in for a
    /// connection it closed without answering, the wait's or the write's
    /// own otherwise. A reply's handles are closed here: the families that
    /// exchange() directly are the ones whose replies carry something.
    [[nodiscard]]
    auto call(wire::Message const &request) -> ustl::Result<wire::Message, ours::Status> {
        auto answered = exchange(m_channel, request, m_reply, m_handles);
        if (answered.is_err()) {
            return answered;
        }
        auto &message = *answered;
        auto const close_handles = [&message] {
            for (auto const handle: message.handles) {
                auto _ = ours::handle_close(handle);
            }
        };
        // A provider that refused the connection closed it with an epitaph,
        // which is a message like any other until it is asked what it is:
        // its status is the call's failure, not a reply to decode.
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToClient);
        if (header.is_err()) {
            close_handles();
            return ustl::err(ours::Status::BadState);
        }
        if (wire::is_epitaph(header.unwrap())) {
            close_handles();
            auto const epitaph = wire::decode_epitaph(message);
            return ustl::err(epitaph.is_err() ? ours::Status::BadState : epitaph.unwrap());
        }
        // The success path closes too: a family whose reply carries a
        // payload handle exchanges directly and takes it from exchange's
        // return -- call()'s contract is that its caller never sees one.
        close_handles();
        message.handles = {};
        return answered;
    }

  private:
    ours::Uandle m_channel;
    u32 m_txid = 0;
    /// The reply's storage: call()'s message views these, valid until the
    /// next call on this channel.
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_reply{};
    std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> m_handles{};
};
} // namespace rpc

#endif // #ifndef RPC_RPC_HPP
