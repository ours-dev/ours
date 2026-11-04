/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROT_FSHOST_CLIENT_HPP
#define PROT_FSHOST_CLIENT_HPP 1

#include <prot/fshost.hpp>

#include <array>
#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <rpc/rpc.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The client side of ours.fshost.Admin, for user programs: the mount
/// control a filesystem host exposes, from a path in this process's
/// namespace.
///
/// The channel is the caller's to open (fsio::connect on the Admin name,
/// as for any protocol); a Client owns the channel it is given and closes
/// it when it dies. One thread at a time, as the buffers are shared.
namespace prot::fshost {
class Client final {
  public:
    /// Takes over |channel|, an open connection to an Admin provider.
    explicit Client(ours::Uandle const channel) : m_channel(channel) {}

    Client(Client const &) = delete;
    auto operator=(Client const &) -> Client & = delete;

    /// Mounts |directory| as |name| in the filesystem host's tree. The
    /// directory handle goes with the message -- a transferred handle
    /// cannot be sent twice. The reply is the status form, whatever the
    /// status: Ok on a fresh tree, AlreadyExists when |name| is taken.
    [[nodiscard]]
    auto mount(ours::Uandle const directory, std::string_view const name) -> ours::Status {
        auto const txid = m_channel.next_txid();
        auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto handles = std::array{directory};
        auto const request =
            encode_mount(bytes, handles, {.txid = txid, .handle = directory, .name = name});
        if (request.is_err()) {
            return request.unwrap_err();
        }
        auto const answered =
            m_channel.call(wire::Message{.bytes = std::span{bytes}.first(request->bytes.size()),
                                         .handles = std::span{handles}.first(1)});
        if (answered.is_err()) {
            return answered.unwrap_err();
        }
        auto const status = wire::decode_reply_status(*answered, MOUNT, txid);
        if (status.is_err()) {
            return ours::Status::InvArgs;
        }
        return status.unwrap();
    }

  private:
    rpc::Channel m_channel;
};
} // namespace prot::fshost

#endif // #ifndef PROT_FSHOST_CLIENT_HPP
