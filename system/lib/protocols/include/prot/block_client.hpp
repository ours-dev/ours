/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROT_BLOCK_CLIENT_HPP
#define PROT_BLOCK_CLIENT_HPP 1

#include <prot/block.hpp>

#include <array>

#include <cstring>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <rpc/rpc.hpp>
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
///   auto connection = fsio::connect(prot::block::BLOCK_NAME);  // the route
///   prot::block::Client block{connection};
///   auto geometry = block.get_info();
///
/// One thread at a time, as the buffers are shared between the calls: the
/// caller serializes, which one Loop thread makes true.
namespace prot::block {
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

    /// The device's geometry, or the failure the provider answered.
    [[nodiscard]]
    auto get_info() -> ustl::Result<Geometry, ours::Status> {
        auto const txid = m_channel.next_txid();
        auto const request = encode_get_info(m_request, {.txid = txid});
        if (request.is_err()) {
            return ustl::err(request.unwrap_err());
        }
        auto reply = m_channel.call(request.unwrap());
        if (reply.is_err()) {
            return ustl::err(reply.unwrap_err());
        }
        auto const answered = decode_get_info_reply(*reply, txid);
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
        auto const txid = m_channel.next_txid();
        auto const request = encode_read(m_request, {.txid = txid, .lba = lba, .count = count});
        if (request.is_err()) {
            return request.unwrap_err();
        }
        auto reply = m_channel.call(request.unwrap());
        if (reply.is_err()) {
            return reply.unwrap_err();
        }
        auto const answered = decode_read_reply(*reply, txid);
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
        auto const txid = m_channel.next_txid();
        auto const request =
            encode_write(m_request, {.txid = txid,
                                     .lba = lba,
                                     .count = static_cast<u32>(data.size() / BLOCK_SIZE),
                                     .data = data});
        if (request.is_err()) {
            return request.unwrap_err();
        }
        auto reply = m_channel.call(request.unwrap());
        if (reply.is_err()) {
            return reply.unwrap_err();
        }
        auto const answered = decode_write_reply(*reply, txid);
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
    rpc::Channel m_channel;
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_request{};
};
} // namespace prot::block

#endif // #ifndef PROT_BLOCK_CLIENT_HPP
