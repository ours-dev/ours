/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_RESOURCE_SERVER_HPP
#define INIT_RESOURCE_SERVER_HPP 1

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <array>

#include "pools.hpp"
#include <loop/loop.hpp>
#include <ours_resource/ours_resource.hpp>
#include <wire/wire.hpp>

/// The root's ours.resource protocols: built-ins (component-framework design,
/// "built-ins", D19; .agents/tasks/microkernel-interrupts/DESIGN.md, D5),
/// through which a component routed one gets the kernel resource it is named
/// after, and the only way to it on dev: no component gets one as a startup
/// handle. One ResourceServer serves one protocol -- Debug, Irq or Ioport --
/// and answers every Get with a duplicate of the resource it was given.
namespace init {
class ResourceServer;

/// One connection of a ResourceServer, as the root's pool keeps it. A program
/// declares an array of them for a ResourceServer to use; only the server
/// reads or writes what is inside. Trivially destructible, so a constinit
/// global of them needs nothing more.
class ResourceConnection final {
  public:
    constexpr ResourceConnection() = default;

  private:
    friend class ResourceServer;

    /// The server whose connection this is, once bind() has taken the entry.
    ResourceServer *m_server = nullptr;
    /// The connection's channel, the Loop's while the entry is in use.
    /// Invalid marks a free entry.
    ours::Uandle m_channel = ours::Uandle::Invalid;
};

/// Serves one ours.resource protocol on the connections a /svc Open routes
/// to it, each on a Loop slot of its own, one message per callback (I4).
/// Single-threaded, as everything in the root (D16).
class ResourceServer final {
  public:
    /// Everything one server keeps, sized by the pools (pools.hpp): a
    /// constinit global of the root's, one per server.
    struct Storage {
        std::array<ResourceConnection, MAX_RESOURCE_CONNECTIONS> connections;
        /// Where a connection's callback reads the one message it handles:
        /// room for the largest a channel carries, so that a client that
        /// sends more than a Get cannot leave a message the read refuses.
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        /// Where Get's reply is encoded: the status form, or the Ok reply
        /// with the resource.
        std::array<u8, wire::STATUS_FORM_SIZE> reply_bytes;
        std::array<ours::Uandle, 1> reply_handles;
    };

    /// A server of |protocol|, one of ours_resource's, whose connections
    /// live in |storage| and wait on |loop|, and which hands out duplicates
    /// of |resource|: the one userboot gave the root for this protocol,
    /// which stays the root's. |storage| and |loop| must outlive it.
    ResourceServer(Storage &storage, loop::Loop &loop, ours_resource::Protocol const &protocol,
                   ours::Uandle resource);

    ResourceServer(ResourceServer const &) = delete;
    auto operator=(ResourceServer const &) -> ResourceServer & = delete;

    /// Serves the protocol on |channel| as a new connection.
    /// The same answers as LauncherServer::bind(): on failure the channel is
    /// still the caller's, NoResource when every connection is in use.
    [[nodiscard]]
    auto bind(ours::Uandle channel) -> ours::Status;

  private:
    /// A connection's slot callback: |context| is its ResourceConnection.
    static auto on_signals(void *context, ours::Signals observed) -> loop::Action;

    /// Handles what |observed| says happened on |connection|: one message,
    /// or its client gone.
    [[nodiscard]]
    auto handle(ResourceConnection &connection, ours::Signals observed) -> loop::Action;

    Storage *m_storage;
    loop::Loop *m_loop;
    ours_resource::Protocol m_protocol;
    ours::Uandle m_resource;
};
} // namespace init

#endif // #ifndef INIT_RESOURCE_SERVER_HPP
