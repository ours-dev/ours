/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_LAUNCHER_SERVER_HPP
#define INIT_LAUNCHER_SERVER_HPP 1

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

#include <array>

#include "environment.hpp"
#include "pools.hpp"
#include <elfload/diagnostics.hpp>
#include <launch/process_launcher.hpp>
#include <loop/loop.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The root's ours.process.Launcher: one of its built-in protocols
/// (component-framework design, "built-ins", S4, D13, D14), reached by a
/// component that uses it through its /svc. It starts programs with
/// launch::start(): it resolves no names, and starts whatever executable VMO
/// a client hands it, under the job the client hands with it.
namespace init {
class LauncherServer;

/// One Launcher connection, as the root's pool keeps it: its channel, the
/// server it belongs to, and what its Add* calls have given so far
/// (launch::ProcessLauncherState). A program declares an array of them for
/// a LauncherServer to use; only the server reads or writes what is inside.
///
/// The pool is a constinit global of the root's, and [[clang::no_destroy]]:
/// nothing runs a user program's dynamic initialisers, and the runtime
/// defines no __cxa_atexit, so the state's hooks must be in place at compile
/// time and its destructor must never be registered. The hooks' context is
/// the connection itself, whose address is a constant there, rather than
/// the server, which main() builds at run time; the connection finds its
/// server through m_server, which bind() sets.
class LauncherConnection final {
  public:
    constexpr LauncherConnection()
        : m_state(launch::LauncherOps{
              .start = &LauncherConnection::start,
              .close = &LauncherConnection::close,
              .context = this,
          }) {}

    // The state's hooks point at this entry, so it never moves.
    LauncherConnection(LauncherConnection const &) = delete;
    auto operator=(LauncherConnection const &) -> LauncherConnection & = delete;

  private:
    friend class LauncherServer;

    /// The state's start hook: LauncherServer::start().
    static auto start(void *context, launch::Startup const &startup)
        -> ustl::Result<ours::Uandle, ours::Status>;

    /// The state's close hook: closes |handle|.
    static auto close(void *context, ours::Uandle handle) -> void;

    /// The server whose connection this is, once bind() has taken the entry.
    LauncherServer *m_server = nullptr;
    /// The connection's channel, the Loop's while the entry is in use.
    /// Invalid marks a free entry.
    ours::Uandle m_channel = ours::Uandle::Invalid;
    launch::ProcessLauncherState m_state;
};

/// Serves ours.process.Launcher on the connections a /svc Open routes
/// to it, each on a Loop slot of its own, one message per callback (I4).
///
/// Single-threaded, as everything in the root (D16): one callback runs at a
/// time, so one set of buffers serves every connection, and nothing locks.
class LauncherServer final {
  public:
    /// Everything the server keeps, sized by the pools (pools.hpp): a
    /// constinit, [[clang::no_destroy]] global of the root's (see
    /// LauncherConnection), about 28 KiB, too much for the stack.
    struct Storage {
        std::array<LauncherConnection, MAX_LAUNCHER_CONNECTIONS> connections;
        /// Where a connection's callback reads the one message it handles.
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        /// Where a Launch's processargs is encoded
        /// (ProcessLauncherState::receive()'s scratch).
        std::array<u8, ours::CHANNEL_MAX_MSG_BYTES> process_args;
        /// Where a Launch's reply is encoded: the 24-byte status form, or
        /// the Ok reply with the process.
        std::array<u8, wire::STATUS_FORM_SIZE> reply_bytes;
        std::array<ours::Uandle, 1> reply_handles;
    };

    /// A server whose connections live in |storage| and wait on |loop|, and
    /// which starts programs with what |environment| gives (the job aside:
    /// each Launch brings its own), reporting launch::start()'s failures to
    /// |diagnostics|. |storage| and |loop| must outlive it.
    LauncherServer(Storage &storage, loop::Loop &loop, Environment const &environment,
                   elfload::Diagnostics diagnostics);

    LauncherServer(LauncherServer const &) = delete;
    auto operator=(LauncherServer const &) -> LauncherServer & = delete;

    /// Serves ours.process.Launcher on |channel| as a new connection,
    /// with nothing added yet: what a /svc Open routed to the built-in
    /// becomes (design "Events: a component connecting to a protocol
    /// through /svc", step 3). On Ok the channel is the server's, its slot's
    /// to close. On failure nothing changed and the channel is still the
    /// caller's, to close with an epitaph: NoResource when every connection
    /// is in use (D27), or the Loop's refusal.
    [[nodiscard]]
    auto bind(ours::Uandle channel) -> ours::Status;

  private:
    friend class LauncherConnection;

    /// A connection's slot callback: |context| is its LauncherConnection.
    static auto on_signals(void *context, ours::Signals observed) -> loop::Action;

    /// Handles what |observed| says happened on |connection|: one message,
    /// or its client gone.
    [[nodiscard]]
    auto handle(LauncherConnection &connection, ours::Signals observed) -> loop::Action;

    /// Frees |connection|'s entry as its slot closes: everything the client
    /// added and the state still holds is closed (reset()), and the entry
    /// can be bound again. The channel is the slot's, which closes it.
    static auto release(LauncherConnection &connection) -> void;

    /// Starts the program a Launch asked for, which the state has judged:
    /// launch::start() on the client's VMO under the client's job, then the
    /// processargs the state encoded, with the client's handles and the
    /// launcher's own two, VmaRoot and VmoVdso (D19). Takes every handle of
    /// |startup.handles|, whatever it answers. The process on Ok.
    [[nodiscard]]
    auto start(launch::Startup const &startup) -> ustl::Result<ours::Uandle, ours::Status>;

    Storage *m_storage;
    loop::Loop *m_loop;
    Environment m_environment;
    elfload::Diagnostics m_diagnostics;
};
} // namespace init

#endif // #ifndef INIT_LAUNCHER_SERVER_HPP
