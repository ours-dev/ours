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
#include <rpc/rpc.hpp>
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
        auto const txid = m_channel.next_txid();
        auto const request = encode_publish(m_request, m_request_handles,
                                            {.txid = txid,
                                             .klass = klass,
                                             .name = name,
                                             .protocol = protocol,
                                             .directory = directory});
        if (request.is_err()) {
            return request.unwrap_err();
        }
        auto reply = m_channel.call(request.unwrap());
        if (reply.is_err()) {
            return reply.unwrap_err();
        }
        auto const answered = decode_publish_reply(*reply, txid);
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
    rpc::Channel m_channel;
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_request{};
    /// The publish carries one handle, the directory.
    std::array<ours::Uandle, 1> m_request_handles{};
};
} // namespace prot::dev

#endif // #ifndef PROT_DEV_CLIENT_HPP
