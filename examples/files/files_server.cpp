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
///   docs   at /export/docs, which this program serves: three files, one of
///          them in a subdirectory;
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
///     It holds the tree under export/docs and nothing under export/ghost;
///   - a VMO for each file, which this program makes and fills from the
///     constants below;
///   - the service node export/docs/stop, and the handler behind it, which
///     is how a consumer asks this program to exit.
///
/// The events: the root writes one Open on the outgoing directory for each
/// consumer's entry, naming the consumer's rights; the connection's slot
/// fires, ioservice::Server makes a connection on the consumer's channel at
/// export/docs with exactly those rights, and from then on the consumer and
/// the Loop talk directly. A path that names nothing, as export/ghost does,
/// is answered with an epitaph on the consumer's channel, NotFound.
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

#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ustl/lazy_init.hpp>
#include <ustl/result.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// The Loop's slots: one per connection to the outgoing directory. There is
/// no protocol to serve, so nothing else waits.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

/// Where the manifest declares the directory this program serves (files.cm,
/// `docs`). A name here that the manifest spells differently serves a tree
/// nobody opens, and every consumer reads NotFound.
constexpr auto DOCS_PATH = std::string_view{"/export/docs"};

/// The service node under it that stops the program, and what it is called
/// to the consumer: /files/stop, where the dev image puts the entry /files.
constexpr auto STOP_NAME = std::string_view{"stop"};

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

/// Adds the file |name| serving |text| to |parent|; Ok or what failed.
auto add_file(ioservice::Outgoing &outgoing, ioservice::NodeId const parent,
              std::string_view const name, std::string_view const text) -> Status {
    auto const file = make_file(text);
    if (file.is_err()) {
        return file.unwrap_err();
    }
    auto const added = outgoing.add_file(parent, name, file.unwrap());
    if (added.is_err()) {
        // The node was refused, so nothing serves the VMO: it goes with it.
        auto _ = ours::handle_close(file.unwrap().vmo);
        return added.unwrap_err();
    }
    return Status::Ok;
}

/// What an Open of `stop` comes to: this program exits with 0.
///
/// The channel the Open carried is dropped, not closed: the process's death
/// closes it with every other handle, so a consumer that waits for its end to
/// close is waiting for the program to be gone. Closing it first would end
/// that wait while this program was still alive.
///
/// Exiting from inside a Loop callback is this program's exit(): the Loop has
/// no quit, and its run() returns only when the port fails. Everything this
/// process holds closes as it dies -- the outgoing directory, the consumers'
/// connections, the port -- and the root, seeing the outgoing directory and
/// the process go, answers later routes to it NotFound.
class StopHandler final: public ioservice::Handler {
  public:
    auto connect(Uandle) -> void override {
        auto _ = ort::debug_line("files: stopping, as a consumer asked");
        ours::process_exit(0);
    }
};

/// Fills the tree: export/docs holding hello.txt, numbers.txt, the service
/// node stop (which |stop| handles) and sub/, which holds nested.txt. Nothing
/// is added at the path the manifest declares for `ghost`.
auto build_tree(ioservice::Outgoing &outgoing, StopHandler &stop) -> Status {
    auto const docs = outgoing.add_directory(DOCS_PATH);
    if (docs.is_err()) {
        return docs.unwrap_err();
    }
    if (auto const added = outgoing.add_service(docs.unwrap(), STOP_NAME, stop); added.is_err()) {
        return added.unwrap_err();
    }
    if (auto const status = add_file(outgoing, docs.unwrap(), HELLO_NAME, HELLO_TEXT);
        status != Status::Ok) {
        return status;
    }
    if (auto const status = add_file(outgoing, docs.unwrap(), NUMBERS_NAME, NUMBERS_TEXT);
        status != Status::Ok) {
        return status;
    }
    auto const sub = outgoing.add_directory(docs.unwrap(), SUB_NAME);
    if (sub.is_err()) {
        return sub.unwrap_err();
    }
    return add_file(outgoing, sub.unwrap(), NESTED_NAME, NESTED_TEXT);
}

// What the directory keeps, outside the stack: constinit, because nothing
// runs a user program's dynamic initialisers. The outgoing directory's
// storage holds types with destructors (ustl::Option, in the ioservice
// connections), which as a plain global would register them with
// __cxa_atexit, which the runtime does not have; ustl::LazyInit is raw
// storage that main() builds it in instead, as init's are
// (system/service/init/init.cpp).
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
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
    StopHandler stop;
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);

    if (auto const status = build_tree(outgoing, stop); status != Status::Ok) {
        report("cannot build its tree", status);
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
