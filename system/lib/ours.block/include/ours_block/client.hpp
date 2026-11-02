/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_BLOCK_CLIENT_HPP
#define OURS_BLOCK_CLIENT_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <cstring>

#include <ours_block/ours_block.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The client side of ours.block, over one connected channel: the codec's
/// calls with the one-at-a-time plumbing a caller would otherwise re-paste --
/// the txid counter, the two message buffers, the wait and the epitaph check.
///
/// The channel is the caller's to open: the /svc open is component plumbing
/// (fsio::connect on the protocol's name, exactly as for any other
/// protocol), and this library stays bytes and calls alone. A Client owns
/// the channel it is given and closes it when it dies.
///
///   auto connection = open_block_service();   // fsio::connect, the route
///   ours_block::Client block{connection};
///   auto geometry = block.get_info();
///
/// One thread at a time, as the buffers are shared between the calls: the
/// caller serializes, which one Loop thread makes true.
namespace ours_block {
class Client final {
  public:
    /// The device's geometry, from Block.GetInfo.
    struct Geometry {
        u32 block_size = 0;
        u64 blocks = 0;
    };

    /// Takes over |channel|, an open connection to a Block provider.
    explicit Client(ours::Uandle const channel) : m_channel(channel) {}

    Client(Client const &) = delete;
    auto operator=(Client const &) -> Client & = delete;

    ~Client() {
        if (m_channel != ours::Uandle::Invalid) {
            auto _ = ours::handle_close(m_channel);
        }
    }

    /// The device's geometry, or the failure the provider answered.
    [[nodiscard]]
    auto get_info() -> ustl::Result<Geometry, ours::Status> {
        auto const request = encode_get_info(m_request, {.txid = next_txid()});
        if (request.is_err()) {
            return ustl::err(request.unwrap_err());
        }
        auto reply = receive(request.unwrap());
        if (reply.is_err()) {
            return ustl::err(reply.unwrap_err());
        }
        auto const answered = decode_get_info_reply(*reply, m_txid);
        if (answered.is_err()) {
            return ustl::err(answered.unwrap_err());
        }
        if (answered->status != ours::Status::Ok) {
            return ustl::err(answered->status);
        }
        return ustl::ok(Geometry{.block_size = answered->block_size, .blocks = answered->blocks});
    }

    /// Reads |count| blocks starting at |lba| into |out|, whose size must
    /// cover count * BLOCK_SIZE. The devices this protocol reaches so far
    /// have BLOCK_SIZE's block; a device that answers another is refused.
    [[nodiscard]]
    auto read(u64 const lba, u32 const count, std::span<u8> out) -> ours::Status {
        if (out.size() < usize{count} * BLOCK_SIZE) {
            return ours::Status::BufferTooSmall;
        }
        auto const request =
            encode_read(m_request, {.txid = next_txid(), .lba = lba, .count = count});
        if (request.is_err()) {
            return request.unwrap_err();
        }
        auto reply = receive(request.unwrap());
        if (reply.is_err()) {
            return reply.unwrap_err();
        }
        auto const answered = decode_read_reply(*reply, m_txid);
        if (answered.is_err()) {
            return answered.unwrap_err();
        }
        if (answered->status != ours::Status::Ok) {
            return answered->status;
        }
        if (answered->data.size() != usize{count} * BLOCK_SIZE) {
            return ours::Status::BadState;
        }
        std::memcpy(out.data(), answered->data.data(), answered->data.size());
        return ours::Status::Ok;
    }

    /// Writes |data|, whole blocks, starting at |lba|.
    [[nodiscard]]
    auto write(u64 const lba, std::span<u8 const> const data) -> ours::Status {
        if (data.empty() || data.size() % BLOCK_SIZE != 0 ||
            data.size() > MAX_READ_BLOCKS * BLOCK_SIZE) {
            return ours::Status::InvArgs;
        }
        auto const request =
            encode_write(m_request, {.txid = next_txid(),
                                     .lba = lba,
                                     .count = static_cast<u32>(data.size() / BLOCK_SIZE),
                                     .data = data});
        if (request.is_err()) {
            return request.unwrap_err();
        }
        auto reply = receive(request.unwrap());
        if (reply.is_err()) {
            return reply.unwrap_err();
        }
        auto const answered = decode_write_reply(*reply, m_txid);
        if (answered.is_err()) {
            return answered.unwrap_err();
        }
        return answered->status;
    }

    /// What ours.gpt reads through: |count| blocks into |out|, from |lba|.
    /// The shape ours.gpt's Reader asks for, so a Client is a Reader.
    [[nodiscard]]
    auto read_blocks(u64 const lba, usize const count, std::span<u8> out) -> ours::Status {
        if (count == 0 || count > MAX_READ_BLOCKS) {
            return ours::Status::InvArgs;
        }
        return read(lba, static_cast<u32>(count), out);
    }

  private:
    /// Sends |request| and reads the reply, an epitaph's status standing in
    /// for a failure the provider closed the connection with.
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

    auto next_txid() -> u32 {
        m_txid = m_txid % wire::MAX_TXID + 1;
        return m_txid;
    }

    ours::Uandle m_channel;
    u32 m_txid = 0;
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_request{};
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_reply{};
    std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> m_reply_handles{};
};
} // namespace ours_block

#endif // #ifndef OURS_BLOCK_CLIENT_HPP
