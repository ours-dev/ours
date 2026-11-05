/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROT_VIRTIO_CONTROL_CLIENT_HPP
#define PROT_VIRTIO_CONTROL_CLIENT_HPP 1

#include <prot/virtio_control.hpp>

#include <array>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <rpc/rpc.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The client side of ours.virtio.Control, over one connected channel: the
/// codec's rescan with the plumbing -- txid counter, buffers, the wait and
/// the epitaph check -- a caller would otherwise re-paste. The twin of
/// prot::nvme::Client.
///
/// The channel is the caller's to open (fsio::connect on CONTROL_PATH, as
/// for any other protocol); a Client owns the channel it is given and
/// closes it when it dies. One thread at a time, as the buffers are shared
/// between the calls: the caller serializes, which one Loop thread makes
/// true.
///
///   auto connection = fsio::connect(prot::virtio::CONTROL_PATH, server_end);
///   prot::virtio::Client control{connection};
///   control.rescan();
namespace prot::virtio {
class Client final {
  public:
    /// Takes over |channel|, an open connection to the driver's control.
    explicit Client(ours::Uandle const channel) : m_channel(channel) {}

    Client(Client const &) = delete;
    auto operator=(Client const &) -> Client & = delete;

    /// Asks the driver to look at the bus again. Ok when a function was
    /// found and brought up; NotFound when the walk saw none it owns -- an
    /// unchanged machine's answer. Otherwise why not: the epitaph a route
    /// that leads nowhere closed the connection with, PeerClosed for a
    /// connection closed without one, or an answer that is not a
    /// well-formed reply (InvArgs).
    [[nodiscard]]
    auto rescan() -> ours::Status {
        auto const txid = m_channel.next_txid();
        // The request always fits the buffer and the txid is fixed: the
        // encode cannot refuse.
        auto const request = encode_rescan(m_request, {.txid = txid}).unwrap();
        auto reply = m_channel.call(request);
        if (reply.is_err()) {
            return reply.unwrap_err();
        }
        auto const answered = decode_rescan_reply(*reply, txid);
        if (answered.is_err()) {
            return answered.unwrap_err();
        }
        return answered->status;
    }

  private:
    rpc::Channel m_channel;
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_request{};
};
} // namespace prot::virtio

#endif // #ifndef PROT_VIRTIO_CONTROL_CLIENT_HPP
