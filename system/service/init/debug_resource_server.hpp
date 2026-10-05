/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_DEBUG_RESOURCE_SERVER_HPP
#define INIT_DEBUG_RESOURCE_SERVER_HPP 1

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <array>

#include "pools.hpp"
#include <loop/loop.hpp>
#include <wire/wire.hpp>

/// The root's fuchsia.kernel.DebugResource: one of its built-in protocols
/// (component-framework design, "built-ins", D19), through which a component
/// routed the protocol gets the Debug resource, and the only way to it on
/// dev: no component gets it as a startup handle. The counterpart of
/// component_manager's DebugResource builtin
/// (src/sys/component_manager/lib/builtins/src/debug_resource.rs:17-26),
/// which answers every Get with a duplicate of the resource it was given.
namespace init {
class DebugResourceServer;

/// One DebugResource connection, as the root's pool keeps it. A program
/// declares an array of them for a DebugResourceServer to use; only the
/// server reads or writes what is inside. Trivially destructible, so a
/// constinit global of them needs nothing more.
class DebugResourceConnection final {
  public:
    constexpr DebugResourceConnection() = default;

  private:
    friend class DebugResourceServer;

    /// The server whose connection this is, once bind() has taken the entry.
    DebugResourceServer *m_server = nullptr;
    /// The connection's channel, the Loop's while the entry is in use.
    /// Invalid marks a free entry.
    ours::Uandle m_channel = ours::Uandle::Invalid;
};

/// Serves fuchsia.kernel.DebugResource on the connections a /svc Open routes
/// to it, each on a Loop slot of its own, one message per callback (I4).
/// Single-threaded, as everything in the root (D16).
class DebugResourceServer final {
  public:
    /// Everything the server keeps, sized by the pools (pools.hpp): a
    /// constinit global of the root's.
    struct Storage {
        std::array<DebugResourceConnection, MAX_DEBUG_RESOURCE_CONNECTIONS> connections;
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

    /// A server whose connections live in |storage| and wait on |loop|, and
    /// which hands out duplicates of |resource|, the Debug resource userboot
    /// gave the root, which stays the root's. |storage| and |loop| must
    /// outlive it.
    DebugResourceServer(Storage &storage, loop::Loop &loop, ours::Uandle resource);

    DebugResourceServer(DebugResourceServer const &) = delete;
    auto operator=(DebugResourceServer const &) -> DebugResourceServer & = delete;

    /// Serves fuchsia.kernel.DebugResource on |channel| as a new connection.
    /// The same answers as LauncherServer::bind(): on failure the channel is
    /// still the caller's, NoResource when every connection is in use.
    [[nodiscard]]
    auto bind(ours::Uandle channel) -> ours::Status;

  private:
    /// A connection's slot callback: |context| is its
    /// DebugResourceConnection.
    static auto on_signals(void *context, ours::Signals observed) -> loop::Action;

    /// Handles what |observed| says happened on |connection|: one message,
    /// or its client gone.
    [[nodiscard]]
    auto handle(DebugResourceConnection &connection, ours::Signals observed) -> loop::Action;

    Storage *m_storage;
    loop::Loop *m_loop;
    ours::Uandle m_resource;
};
} // namespace init

#endif // #ifndef INIT_DEBUG_RESOURCE_SERVER_HPP
