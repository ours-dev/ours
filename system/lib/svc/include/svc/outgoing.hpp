/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SVC_OUTGOING_HPP
#define SVC_OUTGOING_HPP 1

#include <array>
#include <string_view>

#include <loop/loop.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours_io/ours_io.hpp>
#include <vfs/server.hpp>
#include <vfs/tree.hpp>
#include <wire/wire.hpp>

/// The provider side of a component (component-framework design,
/// "svc::Outgoing"): the outgoing directory a component serves on the
/// DirectoryRequest handle its runner hands it, with one entry under svc/ per
/// protocol it provides. A protocol's channel is handed to the program as
/// it comes, and the program serves it however it likes.
///
/// The objects:
///   - the tree: the outgoing directory, its svc/ directory, and a service
///     node per protocol, all in a vfs::Tree (<vfs/tree.hpp>);
///   - the vfs server and its connection table: the DirectoryRequest
///     channel is the first connection, and an Open or Clone of a directory
///     through it makes another;
///   - one Loop slot per connection, whose callback hands one message to the
///     server and does what its Outcome says;
///   - a Handler per protocol, which an Open of svc/<name> hands the
///     request's channel to.
///
/// The events: the root routes a client's Open of /svc/<name> to this
/// component and writes Open("svc/<name>", PROTOCOL_SERVICE, channel) on the
/// outgoing directory's client end. The connection's slot fires, the vfs
/// server finds the service node, and the protocol's Handler gets the
/// channel. From then on the client and this component talk directly: the
/// Handler usually adds the channel to the same Loop.
///
/// One thread, as the Loop it runs on: nothing is locked, and a callback runs
/// to completion before the next one starts.
namespace svc {
/// How many protocols one outgoing directory may serve.
inline constexpr auto MAX_PROTOCOLS = usize{8};

/// How many connections to the outgoing directory and its svc/ directory
/// may be open at once: the root's own, on the DirectoryRequest channel, and
/// whatever Clone or Open of a directory a client makes through it. An Open
/// of a protocol makes none: its channel goes to the protocol's Handler.
inline constexpr auto MAX_CONNECTIONS = usize{4};

/// The directory protocols live in, inside the outgoing directory: where the
/// root forwards an Open to.
inline constexpr auto SVC_DIRECTORY = std::string_view{"svc"};

/// The rights the outgoing directory is served with: every right ours.io
/// defines. The tree holds no file, so none of them reaches anything to
/// write or run; whoever opens it is held to its own Open's rights.
inline constexpr auto OUTGOING_RIGHTS =
    ours_io::Operations::Connect | ours_io::Operations::ReadBytes |
    ours_io::Operations::WriteBytes | ours_io::Operations::Execute |
    ours_io::Operations::GetAttributes | ours_io::Operations::UpdateAttributes |
    ours_io::Operations::Enumerate | ours_io::Operations::Traverse |
    ours_io::Operations::ModifyDirectory;

/// What an Open of one protocol is handed to: a function and its argument,
/// as vfs::Syscalls and loop::Callback are. |channel| is the request's, and
/// the function owns it from the call on: it serves it, typically by adding
/// it to the Loop, or closes it. It runs inside a Loop callback, so it may
/// add() slots but must not wait.
struct Handler {
    auto (*connect)(void *context, ours::Uandle channel) -> void = nullptr;
    void *context = nullptr;
};

/// One outgoing directory, served on a Loop.
class Outgoing final {
  private:
    /// What a connection's Loop slot is called with: the directory, and the
    /// connection the slot serves.
    struct Binding {
        Outgoing *outgoing = nullptr;
        vfs::ConnectionId connection = {};
    };

  public:
    /// Everything the directory keeps, sized by MAX_PROTOCOLS and
    /// MAX_CONNECTIONS: about 9 KiB, most of it the two message buffers, so
    /// a program declares it constinit, outside its stack. Only the Outgoing
    /// reads or writes what is inside.
    struct Storage {
        /// The outgoing directory, svc/, and a service node per protocol.
        std::array<vfs::Node, 2 + MAX_PROTOCOLS> nodes;
        std::array<vfs::Connection, MAX_CONNECTIONS> connections;
        std::array<Binding, MAX_CONNECTIONS> bindings;
        /// Indexed by the value each service node was added with.
        std::array<Handler, MAX_PROTOCOLS> handlers;
        /// Where a connection's callback reads the one message it handles,
        /// and where the server builds its answer. One of each serves every
        /// callback, since one callback runs at a time.
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        vfs::ReplyBuffer reply;
    };

    /// An empty outgoing directory, holding svc/ and nothing in it, whose
    /// tree and connections live in |storage| and whose connections wait on
    /// |loop|. Both must outlive it. Nothing is served until serve().
    Outgoing(Storage &storage, loop::Loop &loop);

    /// The server and its slots keep pointers to this object.
    Outgoing(Outgoing const &) = delete;
    auto operator=(Outgoing const &) -> Outgoing & = delete;

    /// Serves the protocol |name| at svc/<name>: an Open of it hands the
    /// request's channel to |handler|. It may be called before or after
    /// serve(): an Open finds what is there when
    /// it is handled. |name| is kept as a view, so its bytes must outlive the
    /// directory. Refused with:
    ///   - InvArgs when |name| is not a valid ours.io name
    ///     (vfs::is_valid_name()), or |handler| has no function;
    ///   - AlreadyExists when svc/ has an entry of that name;
    ///   - NoResource once MAX_PROTOCOLS are served.
    [[nodiscard]]
    auto add_protocol(std::string_view name, Handler handler) -> ours::Status;

    /// Serves the outgoing directory on |directory_request|, the server end
    /// of the DirectoryRequest startup handle, with OUTGOING_RIGHTS, and
    /// waits on it. The program takes the handle itself, with
    /// ort::take_startup_handle(DirectoryRequest, 0), and passes it here, so
    /// that this library runs on the host too.
    ///
    /// On Ok the channel is the Loop's, its slot's to close. On failure
    /// nothing changed and the channel is still the caller's: InvArgs for
    /// Uandle::Invalid, NoResource when every connection or every Loop slot
    /// is in use, or what the Loop's add() answered.
    [[nodiscard]]
    auto serve(ours::Uandle directory_request) -> ours::Status;

  private:
    /// A connection slot's callback: |context| is its Binding.
    static auto on_signals(void *context, ours::Signals observed) -> loop::Action;

    /// Handles what |observed| says happened on |connection|: one message,
    /// or its client gone.
    [[nodiscard]]
    auto handle(vfs::ConnectionId connection, ours::Signals observed) -> loop::Action;

    /// Waits on |channel|, the channel of the new connection |connection|.
    /// On failure the connection is released and the channel left to the
    /// caller.
    [[nodiscard]]
    auto watch(ours::Uandle channel, vfs::ConnectionId connection) -> ours::Status;

    Storage *m_storage;
    loop::Loop *m_loop;
    vfs::Tree m_tree;
    vfs::Server m_server;
    vfs::NodeId m_root;
    vfs::NodeId m_svc;
    /// The first m_protocols handlers are in use.
    usize m_protocols = 0;
};
} // namespace svc

#endif // #ifndef SVC_OUTGOING_HPP
