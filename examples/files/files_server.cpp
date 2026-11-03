/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// files_server -- the first component that provides a directory
/// (.agents/tasks/directory-capability/DESIGN.md, "The first provider").
///
/// dev's meta/root.cm starts it as the eager child `files`, and offers its
/// directories from `#files` to `#routing_checks` under other names and with
/// other rights. Its manifest (files.cm, beside this file) declares two:
///
///   docs   at /export/docs, which this program serves itself: three files,
///          one of them in a subdirectory, and a service node that stops it;
///   ghost  at /export/ghost, which this program does not serve, on purpose.
///
/// The path of a declaration is written in the manifest and written again
/// here, and it is the one string the two share: the name `docs` is not in
/// this program at all. The root opens the declared path, joined with the
/// subdirectory every hop of the route stated, when it starts a consumer.
///
/// The objects:
///   - the Loop, one port and a slot per connection, on which everything
///     here waits;
///   - the outgoing directory, ioservice::Outgoing, served on the
///     DirectoryRequest handle the root hands every component with a program.
///     It holds a Subtree node at export/docs and nothing under export/ghost;
///   - DocsTree, the program's own tree for `docs` and the handler behind the
///     Subtree node: a second ioservice::Tree, served by a second
///     ioservice::LoopServer on the same Loop, holding the files and the
///     service node `stop`;
///   - a VMO for each file, which this program makes and fills from the
///     constants below.
///
/// The events: the root writes one Open on the outgoing directory for each
/// consumer's entry, naming the consumer's rights. The outgoing directory's
/// server checks it on its own connection, walks it to the Subtree node
/// export/docs, and hands it to DocsTree with the rest of the path and the
/// Open's flags and channel, and makes no connection of its own. DocsTree
/// forwards the Open, unchanged, to the root connection of its own tree, so
/// that its server judges it as any Open is judged: the connection it makes
/// on the consumer's channel is at the node the rest names and holds exactly
/// the rights the consumer asked for. From then on the consumer and the Loop
/// talk directly. A path that names nothing, as export/ghost does, is
/// answered with an epitaph on the consumer's channel, NotFound, by the
/// outgoing directory; one that names nothing below export/docs is answered
/// the same way, by DocsTree's server.
///
/// A consumer that connects to `stop` through its entry (a use of r* holds
/// CONNECT) is handing this program a channel, and the program exits at once
/// with 0, which the root logs as `init: files exited with 0`. Every
/// connection this program held closes with it, so the consumers see
/// PEER_CLOSED, the entries among them. It is a way for a test to stop a
/// provider at a moment it chooses (routing_checks, the provider-exit case),
/// and it lets any consumer of the directory do the same: acceptable in an
/// example, wrong in a service.
#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>
#include <string_view>

#include <ioservice/loop_server.hpp>
#include <ioservice/outgoing.hpp>
#include <ioservice/server.hpp>
#include <ioservice/tree.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <prot/io.hpp>
#include <ustl/lazy_init.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;
using prot::io::Flags;

/// How many connections DocsTree's server allows at once: the root connection
/// the Opens are forwarded to, a consumer's entry, every directory or file a
/// consumer opens below it and each clone made of an entry. As many as the
/// outgoing directory allowed for the same tree before it was served here.
constexpr auto DOCS_CONNECTIONS = ioservice::Outgoing::MAX_CONNECTIONS;

/// The nodes of DocsTree: the root, hello.txt, numbers.txt, stop, sub and
/// nested.txt are six.
constexpr auto DOCS_NODES = usize{8};

/// The Loop's slots: one per connection to the outgoing directory, which now
/// carries the root's one, and one per connection to DocsTree's server. There
/// is no protocol to serve, so nothing else waits.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + DOCS_CONNECTIONS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

/// The rights of the root connection of DocsTree's tree: every right ours.io
/// defines. The ceiling of an Open that comes through it is not its own but
/// the check the outgoing directory made on the connection the Open came on,
/// which the Open passed before it was handed over (a consumer's rights can
/// only be a subset of the root's, who holds them all).
constexpr auto ROOT_RIGHTS = ioservice::Outgoing::OUTGOING_RIGHTS;

/// Where the manifest declares the directory this program serves (files.cm,
/// `docs`). A name here that the manifest spells differently serves a tree
/// nobody opens, and every consumer reads NotFound.
constexpr auto DOCS_PATH = std::string_view{"/export/docs"};

/// The service node in it that stops the program, and what it is called to the
/// consumer: /files/stop, where the dev image puts the entry /files. Its value
/// is the only one the tree holds, so DocsTree does not read it.
constexpr auto STOP_NAME = std::string_view{"stop"};
constexpr auto STOP_VALUE = u64{0};

/// The files, as constants of the program. A file's VMO is made from these
/// at start, and a Read is answered from them.
constexpr auto HELLO_NAME = std::string_view{"hello.txt"};
constexpr auto HELLO_TEXT = std::string_view{"hello from files\n"};
constexpr auto NUMBERS_NAME = std::string_view{"numbers.txt"};
constexpr auto NUMBERS_TEXT = std::string_view{"one\ntwo\nthree\nfour\n"};
constexpr auto SUB_NAME = std::string_view{"sub"};
constexpr auto NESTED_NAME = std::string_view{"nested.txt"};
constexpr auto NESTED_TEXT = std::string_view{"nested in sub\n"};

/// Says |what| failed with |status|, on the debug console.
auto report(std::string_view const what, Status const status) -> void {
    auto _ = ort::debug_line({"files: ", what, ": ", ours::to_string(status)});
}

/// A file serving |text|: a VMO of its own, written once and never again,
/// and the bytes a Read copies from. |text| must outlive the tree.
auto make_file(std::string_view const text) -> ustl::Result<ioservice::VmoFile, Status> {
    auto vmo = Uandle::Invalid;
    if (auto const status = ours::vmo_create(text.size(), ours::VmoOptions::None, &vmo);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const status = ours::vmo_write(vmo, text.data(), 0, text.size());
        status != Status::Ok) {
        auto _ = ours::handle_close(vmo);
        return ustl::err(status);
    }
    return ustl::ok(ioservice::VmoFile{
        .vmo = vmo,
        .bytes = {reinterpret_cast<u8 const *>(text.data()), text.size()},
        .executable = false,
    });
}

/// Adds the file |name| serving |text| to |parent| of |tree|; Ok or what
/// failed.
auto add_file(ioservice::Tree &tree, ioservice::NodeId const parent, std::string_view const name,
              std::string_view const text) -> Status {
    auto const file = make_file(text);
    if (file.is_err()) {
        return file.unwrap_err();
    }
    auto const added = tree.add_file(parent, name, file.unwrap());
    if (added.is_err()) {
        // The node was refused, so nothing serves the VMO: it goes with it.
        auto _ = ours::handle_close(file.unwrap().vmo);
        return added.unwrap_err();
    }
    return Status::Ok;
}

/// What DocsTree keeps, outside the stack: constinit, because nothing runs a
/// user program's dynamic initialisers. The sizes follow the outgoing
/// directory's own storage, which this is a second of.
struct DocsStorage {
    std::array<ioservice::Node, DOCS_NODES> nodes;
    std::array<ioservice::Connection, DOCS_CONNECTIONS> connections;
    std::array<ioservice::LoopServer::Binding, DOCS_CONNECTIONS> bindings;
    /// Where a connection's callback reads the one message it handles, and
    /// where the server builds its answer.
    std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
    ioservice::ReplyBuffer reply;
    /// Where open() encodes the Open it forwards. Not the reply buffer: that
    /// one is the server's, and this one is written outside its callbacks.
    std::array<u8, wire::MAX_MESSAGE_BYTES> forward_bytes;
};

/// The tree behind export/docs, and the program's side of the Subtree node
/// there.
///
/// The tree is a second ioservice::Tree, served by a second LoopServer on the
/// Loop the outgoing directory is on: the files, a directory of them, and the
/// service node `stop`. An Open the outgoing directory hands over comes to
/// open() with the rest of its path, the Open's own flags and its channel.
///
/// open() does not walk the rest or judge the flags itself. The checks of an
/// Open are the server's, in a fixed order that is private to it (they can
/// only be reached by a message on a connection), and a copy of them here
/// would be a second place for them to differ. So open() writes the Open on
/// the one connection of its tree's server that no consumer holds, its root
/// connection, as the root of the system writes an Open on the outgoing
/// directory: the server walks the rest from the tree's root and makes a
/// connection on the consumer's channel at the node it names, with exactly
/// the rights the flags ask for, or answers the channel an epitaph. The flags
/// are the consumer's own, so what this program makes is never wider than
/// what the outgoing directory let through.
///
/// An Open of `stop`, reached through a consumer's entry, comes to connect()
/// as an Open of a service node does, and the program exits.
class DocsTree final: public ioservice::SubtreeHandler, private ioservice::LoopServer::Delegate {
  public:
    DocsTree(DocsStorage &storage, loop::Loop &loop)
        : m_storage(&storage)
        , m_tree(storage.nodes)
        , m_server(m_tree, loop, ioservice::Syscalls{.handle_duplicate = ours::handle_duplicate},
                   ioservice::LoopServer::Memory{
                       .connections = storage.connections,
                       .bindings = storage.bindings,
                       .request_bytes = storage.request_bytes,
                       .request_handles = storage.request_handles,
                       .reply = &storage.reply,
                   },
                   this) {}

    /// The server and its slots keep pointers to this object.
    DocsTree(DocsTree const &) = delete;
    auto operator=(DocsTree const &) -> DocsTree & = delete;

    /// Fills the tree: its root holding hello.txt, numbers.txt, the service
    /// node stop and sub/, which holds nested.txt. Then serves the root
    /// connection open() forwards to. Ok, or what failed.
    [[nodiscard]]
    auto build() -> Status {
        auto const root = m_tree.add_root();
        if (root.is_err()) {
            return root.unwrap_err();
        }
        auto const docs = root.unwrap();
        if (auto const added = m_tree.add_service(docs, STOP_NAME, STOP_VALUE); added.is_err()) {
            return added.unwrap_err();
        }
        if (auto const status = add_file(m_tree, docs, HELLO_NAME, HELLO_TEXT);
            status != Status::Ok) {
            return status;
        }
        if (auto const status = add_file(m_tree, docs, NUMBERS_NAME, NUMBERS_TEXT);
            status != Status::Ok) {
            return status;
        }
        auto const sub = m_tree.add_directory(docs, SUB_NAME);
        if (sub.is_err()) {
            return sub.unwrap_err();
        }
        if (auto const status = add_file(m_tree, sub.unwrap(), NESTED_NAME, NESTED_TEXT);
            status != Status::Ok) {
            return status;
        }

        // The root connection: this program keeps the client end, and the
        // server is given the other.
        auto here = Uandle::Invalid;
        auto there = Uandle::Invalid;
        if (auto const status =
                ours::channel_create(ours::ChannelCreateOptions::None, &here, &there);
            status != Status::Ok) {
            return status;
        }
        if (auto const status = m_server.serve(there, docs, ROOT_RIGHTS); status != Status::Ok) {
            auto _ = ours::handle_close(here);
            auto _ = ours::handle_close(there);
            return status;
        }
        m_forward = here;
        return Status::Ok;
    }

    /// An Open that walked to export/docs: forwarded to the root connection of
    /// this program's tree, whole. |rest| is "." for the node itself.
    auto open(std::string_view const rest, Flags const flags, Uandle const channel)
        -> void override {
        auto handles = std::array{channel};
        auto const encoded = prot::io::encode_open(
            m_storage->forward_bytes, handles,
            prot::io::OpenRequest{.flags = flags, .path = rest, .object = channel});
        if (encoded.is_err()) {
            // A rest the outgoing directory handed over is a valid path, which
            // fits a message, so this is the buffer's own bug.
            report("cannot encode an Open to forward", encoded.unwrap_err());
            auto _ = ours::handle_close(channel);
            return;
        }
        auto const message = encoded.unwrap();
        // The write takes the channel with it, whatever it answers. A consumer
        // whose Open is lost reads PEER_CLOSED on its entry.
        if (auto const written = ours::channel_write(
                m_forward, ours::ChannelWriteOptions::None, message.bytes.data(),
                static_cast<u32>(message.bytes.size()), message.handles.data(),
                static_cast<u32>(message.handles.size()));
            written != Status::Ok) {
            report("cannot forward an Open", written);
        }
    }

  private:
    /// What an Open of `stop` comes to: this program exits with 0.
    ///
    /// The channel the Open carried is dropped, not closed: the process's
    /// death closes it with every other handle, so a consumer that waits for
    /// its end to close is waiting for the program to be gone. Closing it
    /// first would end that wait while this program was still alive.
    ///
    /// Exiting from inside a Loop callback is this program's exit(): the Loop
    /// has no quit, and its run() returns only when the port fails. Everything
    /// this process holds closes as it dies -- the outgoing directory, the
    /// consumers' connections, the port -- and the root, seeing the outgoing
    /// directory and the process go, answers later routes to it NotFound.
    auto connect(u64, Uandle) -> void override {
        auto _ = ort::debug_line("files: stopping, as a consumer asked");
        ours::process_exit(0);
    }

    DocsStorage *m_storage;
    ioservice::Tree m_tree;
    ioservice::LoopServer m_server;
    /// The client end of the root connection of this program's tree.
    Uandle m_forward = Uandle::Invalid;
};

// What the directories keep, outside the stack: constinit, because nothing
// runs a user program's dynamic initialisers. The outgoing directory's
// storage holds types with destructors (ustl::Option, in the ioservice
// connections), which as a plain global would register them with
// __cxa_atexit, which the runtime does not have; ustl::LazyInit is raw
// storage that main() builds it in instead, as init's are
// (system/service/init/init.cpp).
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<DocsStorage> DOCS_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
} // namespace

auto main(int, char **) -> int {
    // The server end of the outgoing directory, which the root made when it
    // resolved this component and hands every component with a program.
    auto const directory_request = ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0);
    if (directory_request == Uandle::Invalid) {
        auto _ = ort::debug_line("files: started without a DirectoryRequest handle");
        return 1;
    }

    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("cannot make its port", status);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    // Before the Outgoing, which keeps it by address.
    DocsTree docs(*DOCS_STORAGE.init(), loop);
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);

    if (auto const status = docs.build(); status != Status::Ok) {
        report("cannot build its tree", status);
        return 1;
    }
    // The declared path is the Subtree node: every Open that reaches it is
    // DocsTree's.
    if (auto const added = outgoing.add_subtree(DOCS_PATH, docs); added.is_err()) {
        report("cannot serve its tree at its declared path", added.unwrap_err());
        return 1;
    }
    if (auto const status = outgoing.serve(directory_request); status != Status::Ok) {
        report("cannot serve its outgoing directory", status);
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    // Nothing but the stop node ends the program: run() comes back only when
    // the port itself fails.
    report("its Loop stopped", loop.run(ours::INSTANT_INFINITE));
    return 1;
}
