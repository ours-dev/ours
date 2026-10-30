#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>

#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours_examples_hello/ours_examples_hello.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Signals;
using ours::Status;
using ours::Uandle;

/// How many clients may be connected at once. One is all dev's routing
/// checks use; a client past them is refused with NoResource.
constexpr auto MAX_CLIENTS = usize{1};

/// The Loop's slots: one per connection to the outgoing directory, one per
/// client.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

/// Says |what| failed with |status|, on the debug console.
auto report(std::string_view const what, Status const status) -> void {
    auto _ = ort::debug_line({"hello: ", what, ": ", ours::to_string(status)});
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

/// The Hello server: the connections ioservice::Outgoing hands it, each served on
/// a Loop slot of its own, one message per on_signals().
class HelloServer final: public ioservice::Handler {
  private:
    /// One connected client, and the Loop's Waiter for its channel's slot. A
    /// client has one slot and one behaviour, so the Loop calls the client
    /// itself, and the client hands the call to its server.
    ///
    /// The Loop keeps a client's address without owning it (see
    /// loop::Waiter). That holds because the clients lie in Storage, which
    /// never moves, and an entry is taken again only after the Loop has freed
    /// the slot it released: release() answers Close, run() then closes the
    /// channel and frees the slot, and connect(), which runs on the same
    /// thread, finds the entry free only after that.
    class Client final: public loop::Waiter {
      public:
        HelloServer *server = nullptr;
        /// Its channel, the slot's; Invalid while the entry is free.
        Uandle channel = Uandle::Invalid;

      private:
        /// What the Loop calls when the channel has a message or its peer is
        /// gone: the server's serve() of this client. Private, since only the
        /// Loop reaches it, through Waiter.
        auto on_signals(Signals const observed) -> loop::Action override {
            return server->serve(*this, observed);
        }
    };

  public:
    /// Everything the server keeps: about 8 KiB, most of it the two message
    /// buffers, so main() builds it outside its stack.
    struct Storage {
        std::array<Client, MAX_CLIENTS> clients;
        /// Where a client's on_signals() reads the one message it handles,
        /// and where it builds the reply. One of each serves every client,
        /// since one on_signals() runs at a time.
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        std::array<u8, wire::MAX_MESSAGE_BYTES> reply_bytes;
    };

    /// A server with no client, keeping its state in |storage| and waiting
    /// on |loop|. Both must outlive it.
    HelloServer(Storage &storage, loop::Loop &loop) : m_storage(&storage), m_loop(&loop) {}

    HelloServer(HelloServer const &) = delete;
    auto operator=(HelloServer const &) -> HelloServer & = delete;

  private:
    /// Takes |channel|, a new client's, into a free entry and a slot; closes
    /// it with an epitaph when there is neither. ioservice::Outgoing hands an Open
    /// of the protocol here.
    auto connect(Uandle const channel) -> void override {
        for (auto &client: m_storage->clients) {
            if (client.channel != Uandle::Invalid) {
                continue;
            }
            // A Waiter is no aggregate and its copy is not public, so the
            // entry is filled field by field. It is in use once channel is
            // set, so the server goes first.
            client.server = this;
            client.channel = channel;
            if (auto const added =
                    m_loop->add(channel, Signals::Readable | Signals::PeerClosed, client);
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
        // <ours_examples_hello/ours_examples_hello.hpp> lays down. Neither
        // method takes a handle, so any the message brought are closed with
        // it.
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        if (header.is_ok() && header->ordinal == ours_examples_hello::GREET.ordinal) {
            if (auto const request = ours_examples_hello::decode_greet(message); request.is_ok()) {
                return greet(client, *request);
            }
        } else if (header.is_ok() && header->ordinal == ours_examples_hello::STOP.ordinal) {
            if (auto const request = ours_examples_hello::decode_stop(message); request.is_ok()) {
                stop(client, *request);
            }
        }
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
        return release(client);
    }

    /// Greet: the string back, unchanged.
    auto greet(Client &client, ours_examples_hello::GreetRequest const &request) -> loop::Action {
        // The value views the request buffer, and the reply is built in the
        // other one, so the copy never overlaps.
        auto const reply = ours_examples_hello::encode_greet_reply(
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
    /// Exiting from inside an on_signals() is this program's exit(): the Loop has
    /// no quit, and its run() returns only when the port fails. Everything
    /// this process holds closes as it dies -- the outgoing directory, the
    /// client channels, the port -- and the root, seeing the outgoing
    /// directory and the process go, answers later routes to it NotFound
    /// (D23).
    [[noreturn]]
    auto stop(Client const &client, ours_examples_hello::StopRequest const &request) -> void {
        if (auto const reply = ours_examples_hello::encode_stop_reply(m_storage->reply_bytes,
                                                                      request.txid, Status::Ok);
            reply.is_ok()) {
            auto _ = write(client.channel, reply.unwrap());
        }
        auto _ = ort::debug_line("hello: stopping, as a client asked");
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
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<HelloServer::Storage> HELLO_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
} // namespace

auto main(int, char **) -> int {
    // The server end of the outgoing directory, which the root made when it
    // resolved this component and hands every component with a program.
    auto const directory_request = ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0);
    if (directory_request == Uandle::Invalid) {
        auto _ = ort::debug_line("hello: started without a DirectoryRequest handle");
        return 1;
    }

    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("cannot make its port", status);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);
    HelloServer server(*HELLO_STORAGE.init(), loop);

    if (auto const status = outgoing.add_protocol(ours_examples_hello::HELLO_NAME, server);
        status != Status::Ok) {
        report("cannot add its protocol", status);
        return 1;
    }
    if (auto const status = outgoing.serve(directory_request); status != Status::Ok) {
        report("cannot serve its outgoing directory", status);
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    // Stop ends the program from inside an on_signals(); run() comes back only
    // when the port itself fails.
    report("its Loop stopped", loop.run(ours::INSTANT_INFINITE));
    return 1;
}
