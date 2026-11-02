/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SERVER_SERVER_HPP
#define SERVER_SERVER_HPP 1

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>

#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <wire/wire.hpp>

/// The plumbing every protocol server shares, so a new server starts at its
/// methods: the client set, the message pump, and the reply and epitaph
/// helpers, on a Loop.
///
/// The shape is the echo server's (examples/echo, developer's guide chapter
/// 2), which the block device driver repeated, and which a third server
/// would have copied again: a fixed set of client entries, each a Loop
/// Waiter over one channel; a fixed set of buffers, one message read and one
/// reply built at a time; a pump that reads a message, decodes its header
/// and hands it to the protocol's dispatch.
///
/// The objects, in a server that uses this base:
///   - the Storage, which the derived server aliases and main() builds
///     outside its stack: the client entries, and the two message buffers;
///   - the Loop, on which the outgoing directory and every client wait;
///   - the derived server, which owns whatever its methods touch.
///
/// The events: the outgoing directory hands an Open's channel to connect(),
/// which takes a free entry and arms a wait. A message on the channel reads
/// one message, decodes its header, and calls the derived server's
/// on_message(); its answer says whether the connection stays or ends. A
/// peer gone, a read that failed, or a reply that could not be written ends
/// the connection the same way.
///
/// One thread, as the Loop it runs on: an on_message() runs to completion
/// before the next one starts, so a server may run its method's work inline.
///
/// A reply does not have to go out inside the on_message() that received the
/// request. A server may park it: copy what it still needs -- the channel,
/// the txid, the request's payload -- out of the shared buffers into storage
/// of its own, and build the reply later, in a later callback on the same
/// Loop, writing it with reply() as usual. The shared buffers are transient
/// within one callback: the next channel_read overwrites them, so anything a
/// parked request still needs has been copied out by then. A reply that
/// fails because the client went away while its request was parked is
/// dropped silently; the client is gone either way. ioservice's LoopServer
/// states the same rule for a parked Open; the block-server library is the
/// first server built on it.
namespace server {
template <typename Derived, usize MAX_CLIENTS>
class ProtocolServer: public ioservice::Handler {
  public:
    /// One connected client, and the Loop's Waiter for its channel's slot.
    /// A client has one slot and one behaviour, so the Loop calls the client
    /// itself, and the client hands the call back to the base, which hands
    /// it to the derived server.
    ///
    /// The Loop keeps a client's address without owning it (see
    /// loop::Waiter). That holds because the clients lie in the derived
    /// server's Storage, which never moves, and an entry is taken again only
    /// after the Loop has freed the slot it released: release() answers
    /// Close, run() then closes the channel and frees the slot, and
    /// connect(), which runs on the same thread, finds the entry free only
    /// after that. When the wait cannot be armed again, the Loop closes the
    /// channel and frees the slot first, and on_rearm_failed() frees the
    /// entry after, so the order is the same.
    class Client final: public loop::Waiter {
      public:
        ProtocolServer *server = nullptr;
        /// Its channel, the slot's; Invalid while the entry is free.
        ours::Uandle channel = ours::Uandle::Invalid;

      private:
        auto on_signals(ours::Signals const observed) -> loop::Action override {
            return server->serve_client(*this, observed);
        }

        auto on_rearm_failed(ours::Status const) -> void override {
            server->rearm_client(*this);
        }
    };

    /// What a server keeps: the client entries, and the two message buffers
    /// -- where a client's on_message() reads the one message it handles,
    /// and where it builds the reply. One of each serves every client, since
    /// one on_message() runs at a time.
    struct Storage {
        std::array<Client, MAX_CLIENTS> clients;
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        std::array<u8, wire::MAX_MESSAGE_BYTES> reply_bytes;
    };

    /// A server with no client, keeping its state in |storage| and waiting
    /// on |loop|. Both must outlive it, as must whatever the derived server
    /// keeps.
    explicit ProtocolServer(Storage &storage, loop::Loop &loop)
        : m_storage(&storage), m_loop(&loop) {}

    ProtocolServer(ProtocolServer const &) = delete;
    auto operator=(ProtocolServer const &) -> ProtocolServer & = delete;

  protected:
    /// Writes |message| on |channel|; what channel_write answers. A reply
    /// that cannot be written leaves its client waiting for good, so the
    /// caller ends the connection.
    [[nodiscard]]
    auto reply(ours::Uandle const channel, wire::Message const message) const -> ours::Status {
        return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                                   static_cast<u32>(message.bytes.size()), message.handles.data(),
                                   static_cast<u32>(message.handles.size()));
    }

    /// Closes |channel| with an epitaph carrying |status|: how a connection
    /// this server cannot take ends. The client may be gone already; the
    /// channel closes either way.
    auto close_with_epitaph(ours::Uandle const channel, ours::Status const status) const -> void {
        std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
        if (auto const epitaph = wire::encode_epitaph(bytes, status); epitaph.is_ok()) {
            auto _ = reply(channel, epitaph.unwrap());
        }
        auto _ = ours::handle_close(channel);
    }

    /// Closes every handle a refused message carried, which nothing took.
    static auto close_handles(wire::Message const &message) -> void {
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
    }

    /// Ends the client |channel|'s connection: its entry is freed, and the
    /// returned Close makes the Loop close the channel and free the slot.
    auto close_client(ours::Uandle const channel) -> loop::Action {
        for (auto &client: m_storage->clients) {
            if (client.channel != channel) {
                continue;
            }
            client.channel = ours::Uandle::Invalid;
            break;
        }
        return loop::Action::Close;
    }

    /// Where the derived server builds a reply.
    [[nodiscard]]
    auto reply_buffer() const -> std::span<u8> {
        return m_storage->reply_bytes;
    }

  private:
    auto derived() -> Derived * {
        return static_cast<Derived *>(this);
    }

    /// Takes |channel|, a new client's, into a free entry and a slot; closes
    /// it with an epitaph when there is neither. ioservice::Outgoing hands an
    /// Open of the protocol here.
    auto connect(ours::Uandle const channel) -> void override {
        for (auto &client: m_storage->clients) {
            if (client.channel != ours::Uandle::Invalid) {
                continue;
            }
            // A Waiter is no aggregate and its copy is not public, so the
            // entry is filled field by field. It is in use once channel is
            // set, so the server goes first.
            client.server = this;
            client.channel = channel;
            if (auto const added = m_loop->add(
                    channel, ours::Signals::Readable | ours::Signals::PeerClosed, client);
                added.is_err()) {
                client.channel = ours::Uandle::Invalid;
                close_with_epitaph(channel, added.unwrap_err());
            }
            return;
        }
        close_with_epitaph(channel, ours::Status::NoResource);
    }

    /// Handles what |observed| says happened on |client|'s channel: one
    /// message, or the client gone. The pump: one message read, its header
    /// decoded, the derived server's on_message() called with both. A header
    /// that fails to decode means the peer is not speaking the protocol, so
    /// the connection ends without an epitaph, the handles it brought closed
    /// with it.
    auto serve_client(Client &client, ours::Signals const observed) -> loop::Action {
        if (!(observed & ours::Signals::Readable)) {
            return release(client);
        }
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        if (auto const read = ours::channel_read(
                client.channel, ours::ChannelReadOptions::None, m_storage->request_bytes.data(),
                m_storage->request_handles.data(),
                static_cast<u32>(m_storage->request_bytes.size()),
                static_cast<u32>(m_storage->request_handles.size()), &num_bytes, &num_handles);
            read != ours::Status::Ok) {
            return release(client);
        }
        auto message = wire::Message{
            .bytes = std::span{m_storage->request_bytes}.first(num_bytes),
            .handles = std::span{m_storage->request_handles}.first(num_handles),
        };
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        if (header.is_err()) {
            close_handles(message);
            return release(client);
        }
        return derived()->on_message(client.channel, header.unwrap(), std::move(message));
    }

    /// Frees |client|'s entry; its slot closes the channel, by answering
    /// Close.
    auto release(Client &client) -> loop::Action {
        client.channel = ours::Uandle::Invalid;
        return loop::Action::Close;
    }

    /// Frees |client|'s entry after the Loop failed to arm its wait again.
    /// The Loop has closed the channel and freed the slot, so the entry is
    /// all that is left to free.
    auto rearm_client(Client &client) -> void {
        client.channel = ours::Uandle::Invalid;
    }

    Storage *m_storage;
    loop::Loop *m_loop;
};
} // namespace server

#endif // #ifndef SERVER_SERVER_HPP
