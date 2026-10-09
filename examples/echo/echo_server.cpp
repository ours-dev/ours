/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// echo_server -- a component that provides ours.examples.echo.Echo to a
/// sibling (component-framework plan, step 17; design D3 a, D24).
///
/// dev's meta/root.cm starts it as the eager child `echo`, and offers its
/// protocol from `#echo` to `#routing_checks` under another name. That is the
/// route this program exists for: until it, routes through `expose` and the
/// forwarding of an Open into a provider's outgoing directory were tested only
/// on the host.
///
/// The objects:
///   - the Loop, one port and a slot per connection, on which everything
///     here waits;
///   - the outgoing directory, svc::Outgoing, served on the DirectoryRequest
///     handle the root hands every component with a program, holding
///     svc/ours.examples.echo.Echo;
///   - the Echo server: one client per channel an Open of that entry brought,
///     each a Loop slot.
///
/// The events: the root forwards a client's Open into the outgoing directory;
/// svc::Outgoing hands the channel to EchoServer::connect(), which gives it a
/// slot. EchoString is answered with the string it carried. Stop is answered
/// Ok, and the program exits with 0, which the root logs as `init: echo
/// exited with 0`; from then on, a route to it answers NotFound (design D23).
#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>

#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours_examples_echo/ours_examples_echo.hpp>
#include <svc/outgoing.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Signals;
using ours::Status;
using ours::Uandle;

/// How many clients may be connected at once. One is all dev's routing
/// checks use; a client past them is refused with NoResource.
constexpr auto MAX_CLIENTS = usize{4};

/// The Loop's slots: one per connection to the outgoing directory, one per
/// client.
constexpr auto SLOTS = svc::MAX_CONNECTIONS + MAX_CLIENTS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

/// Says |what| failed with |status|, on the debug console.
auto report(std::string_view const what, Status const status) -> void {
    auto _ = ort::debug_line({"echo: ", what, ": ", ours::to_string(status)});
}

/// Writes |message| on |channel|; what channel_write answers.
auto write(Uandle const channel, wire::Message const message) -> Status {
    return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                               static_cast<u32>(message.bytes.size()), message.handles.data(),
                               static_cast<u32>(message.handles.size()));
}

/// Closes |channel| with an epitaph carrying |status|, the way a connection
/// this server cannot take ends.
auto close_with_epitaph(Uandle const channel, Status const status) -> void {
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    if (auto const epitaph = wire::encode_epitaph(bytes, status); epitaph.is_ok()) {
        // The client may be gone already; the channel closes either way.
        auto _ = write(channel, epitaph.unwrap());
    }
    auto _ = ours::handle_close(channel);
}

/// The Echo server: the connections svc::Outgoing hands it, each served on
/// a Loop slot of its own, one message per callback.
class EchoServer final {
  private:
    /// One connected client: what its slot's callback is called with.
    struct Client {
        EchoServer *server = nullptr;
        /// Its channel, the slot's; Invalid while the entry is free.
        Uandle channel = Uandle::Invalid;
    };

  public:
    /// Everything the server keeps: about 8 KiB, most of it the two message
    /// buffers, so main() builds it outside its stack.
    struct Storage {
        std::array<Client, MAX_CLIENTS> clients;
        /// Where a client's callback reads the one message it handles, and
        /// where it builds the reply. One of each serves every callback,
        /// since one callback runs at a time.
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        std::array<u8, wire::MAX_MESSAGE_BYTES> reply_bytes;
    };

    /// A server with no client, keeping its state in |storage| and waiting
    /// on |loop|. Both must outlive it.
    EchoServer(Storage &storage, loop::Loop &loop) : m_storage(&storage), m_loop(&loop) {}

    EchoServer(EchoServer const &) = delete;
    auto operator=(EchoServer const &) -> EchoServer & = delete;

    /// What svc::Outgoing hands an Open of the protocol to.
    [[nodiscard]]
    auto handler() -> svc::Handler {
        return {.connect = &EchoServer::on_connect, .context = this};
    }

  private:
    static auto on_connect(void *const context, Uandle const channel) -> void {
        static_cast<EchoServer *>(context)->connect(channel);
    }

    static auto on_signals(void *const context, Signals const observed) -> loop::Action {
        auto &client = *static_cast<Client *>(context);
        return client.server->serve(client, observed);
    }

    /// Takes |channel|, a new client's, into a free entry and a slot; closes
    /// it with an epitaph when there is neither.
    auto connect(Uandle const channel) -> void {
        for (auto &client: m_storage->clients) {
            if (client.channel != Uandle::Invalid) {
                continue;
            }
            client = Client{.server = this, .channel = channel};
            if (auto const added = m_loop->add(channel, Signals::Readable | Signals::PeerClosed,
                                               &EchoServer::on_signals, &client);
                added.is_err()) {
                client.channel = Uandle::Invalid;
                report("cannot wait on a client", added.unwrap_err());
                close_with_epitaph(channel, added.unwrap_err());
            }
            return;
        }
        close_with_epitaph(channel, Status::NoResource);
    }

    /// Frees |client|'s entry; its slot closes the channel, by answering
    /// Close.
    static auto release(Client &client) -> loop::Action {
        client.channel = Uandle::Invalid;
        return loop::Action::Close;
    }

    /// Handles what |observed| says happened on |client|'s channel: one
    /// message, or the client gone.
    auto serve(Client &client, Signals const observed) -> loop::Action {
        if (!(observed & Signals::Readable)) {
            return release(client);
        }
        auto &storage = *m_storage;
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        if (auto const read = ours::channel_read(
                client.channel, ours::ChannelReadOptions::None, storage.request_bytes.data(),
                storage.request_handles.data(), static_cast<u32>(storage.request_bytes.size()),
                static_cast<u32>(storage.request_handles.size()), &num_bytes, &num_handles);
            read != Status::Ok) {
            return release(client);
        }
        auto const message = wire::Message{
            .bytes = std::span{storage.request_bytes}.first(num_bytes),
            .handles = std::span{storage.request_handles}.first(num_handles),
        };

        // A closed protocol: an ordinal it does not know, or a message its
        // decoder refuses, closes the connection without an epitaph, as
        // <ours_examples_echo/ours_examples_echo.hpp> lays down. Neither
        // method takes a handle, so any the message brought are closed with
        // it.
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        if (header.is_ok() && header->ordinal == ours_examples_echo::ECHO_STRING.ordinal) {
            if (auto const request = ours_examples_echo::decode_echo_string(message);
                request.is_ok()) {
                return echo_string(client, *request);
            }
        } else if (header.is_ok() && header->ordinal == ours_examples_echo::STOP.ordinal) {
            if (auto const request = ours_examples_echo::decode_stop(message); request.is_ok()) {
                stop(client, *request);
            }
        }
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
        return release(client);
    }

    /// EchoString: the string back, unchanged.
    auto echo_string(Client &client, ours_examples_echo::EchoStringRequest const &request)
        -> loop::Action {
        // The value views the request buffer, and the reply is built in the
        // other one, so the copy never overlaps.
        auto const reply = ours_examples_echo::encode_echo_string_reply(
            m_storage->reply_bytes, request.txid,
            {.status = Status::Ok, .response = request.value});
        if (reply.is_err()) {
            report("cannot encode a reply", reply.unwrap_err());
            return release(client);
        }
        // A reply that cannot be written leaves its client waiting for good,
        // so the connection goes with it.
        if (auto const written = write(client.channel, reply.unwrap()); written != Status::Ok) {
            return release(client);
        }
        return loop::Action::Rearm;
    }

    /// Stop: answers Ok, then exits with 0, whether or not the answer could
    /// be written: the client asked, and may already be gone.
    ///
    /// Exiting from inside a callback is this program's exit(): the Loop has
    /// no quit, and its run() returns only when the port fails. Everything
    /// this process holds closes as it dies -- the outgoing directory, the
    /// client channels, the port -- and the root, seeing the outgoing
    /// directory and the process go, answers later routes to it NotFound
    /// (D23).
    [[noreturn]]
    auto stop(Client const &client, ours_examples_echo::StopRequest const &request) -> void {
        if (auto const reply = ours_examples_echo::encode_stop_reply(m_storage->reply_bytes,
                                                                     request.txid, Status::Ok);
            reply.is_ok()) {
            auto _ = write(client.channel, reply.unwrap());
        }
        auto _ = ort::debug_line("echo: stopping, as a client asked");
        ours::process_exit(0);
    }

    Storage *m_storage;
    loop::Loop *m_loop;
};

// What the server keeps, outside the stack: constinit, because nothing runs
// a user program's dynamic initialisers. The outgoing directory's storage
// holds types with destructors (ustl::Option, in the ioservice connections), which
// as a plain global would register them with __cxa_atexit, which the runtime
// does not have; ustl::LazyInit is raw storage that main() builds it in
// instead, as init's are (system/service/init/init.cpp).
constinit ustl::LazyInit<svc::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<EchoServer::Storage> ECHO_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
} // namespace

auto main(int, char **) -> int {
    // The server end of the outgoing directory, which the root made when it
    // resolved this component and hands every component with a program.
    auto const directory_request = ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0);
    if (directory_request == Uandle::Invalid) {
        auto _ = ort::debug_line("echo: started without a DirectoryRequest handle");
        return 1;
    }

    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("cannot make its port", status);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    svc::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);
    EchoServer server(*ECHO_STORAGE.init(), loop);

    if (auto const status = outgoing.add_protocol(ours_examples_echo::ECHO_NAME, server.handler());
        status != Status::Ok) {
        report("cannot add its protocol", status);
        return 1;
    }
    if (auto const status = outgoing.serve(directory_request); status != Status::Ok) {
        report("cannot serve its outgoing directory", status);
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    // Stop ends the program from inside a callback; run() comes back only
    // when the port itself fails.
    report("its Loop stopped", loop.run(ours::INSTANT_INFINITE));
    return 1;
}
