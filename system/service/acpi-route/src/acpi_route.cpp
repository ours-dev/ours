/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// acpi-route -- the ACPI route service: the one program that reads the
/// firmware's tables for the rest of user space, and answers, from the static
/// ones, which GSI an ISA interrupt arrives on and where a PCI segment's
/// configuration space is (ours.acpi.Route). Design:
/// .agents/tasks/driver-manager/DESIGN.md, the route service.
///
/// The objects:
///   - the tables, the read-only VMO the root hands out as ours.boot.Acpi,
///     mapped once and read in place (acpi_route::TableDirectory), which has
///     checked every byte it will hand out;
///   - the Loop, one port and a slot per connection, on which everything here
///     waits;
///   - the outgoing directory, ioservice::Outgoing, served on the
///     DirectoryRequest handle the root hands every component with a program,
///     holding svc/ours.acpi.Route;
///   - the Route server: one client per channel an Open of that entry brought,
///     each a Loop slot.
///
/// The events: the root forwards a client's Open into the outgoing directory;
/// ioservice::Outgoing hands the channel to RouteServer::connect(), which gives
/// it a slot. Each request is answered from the tables and nothing else: this
/// program holds no resource but the VMO, so a crafted table can make it lie,
/// hang or die, and can reach no hardware. Without tables it has nothing to
/// be, and exits; a route to it then answers NotFound.
#include <ours/interrupt.hpp>
#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>

#include <acpi_route/routes.hpp>
#include <acpi_route/table_directory.hpp>
#include <driver_kit/driver_kit.hpp>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours_acpi/ours_acpi.hpp>
#include <ours_resource/client.hpp>
#include <ours_resource/ours_resource.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Signals;
using ours::Status;
using ours::Uandle;

/// How many clients may be connected at once. A client past them is refused
/// with NoResource.
constexpr auto MAX_CLIENTS = usize{4};

/// The Loop's slots: one per connection to the outgoing directory, one per
/// client.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

/// Says |what| failed with |status|, on the debug console.
auto report(std::string_view const what, Status const status) -> void {
    auto _ = ort::debug_line({"acpi-route: ", what, ": ", ours::to_string(status)});
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

constexpr auto wire_trigger(acpi_route::Trigger const trigger) -> ours_acpi::Trigger {
    return trigger == acpi_route::Trigger::Level ? ours_acpi::Trigger::Level
                                                 : ours_acpi::Trigger::Edge;
}

constexpr auto wire_polarity(acpi_route::Polarity const polarity) -> ours_acpi::Polarity {
    return polarity == acpi_route::Polarity::ActiveLow ? ours_acpi::Polarity::ActiveLow
                                                       : ours_acpi::Polarity::ActiveHigh;
}

/// The Route server: the connections ioservice::Outgoing hands it, each served
/// on a Loop slot of its own, one message per on_signals().
class RouteServer final: public ioservice::Handler {
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
    /// thread, finds the entry free only after that. When run() cannot arm the
    /// wait again it closes the channel and frees the slot first and then calls
    /// on_rearm_failed(), which frees the entry, so the order is the same.
    class Client final: public loop::Waiter {
      public:
        RouteServer *server = nullptr;
        /// Its channel, the slot's; Invalid while the entry is free.
        Uandle channel = Uandle::Invalid;

      private:
        /// What the Loop calls when the channel has a message or its peer is
        /// gone: the server's serve() of this client. Private, since only the
        /// Loop reaches it, through Waiter.
        auto on_signals(Signals const observed) -> loop::Action override {
            return server->serve(*this, observed);
        }

        /// What the Loop calls when it could not arm the wait again, after it
        /// has closed the channel and freed the slot: the server's
        /// rearm_failed() of this client. Private for the same reason.
        auto on_rearm_failed(Status const cause) -> void override {
            server->rearm_failed(*this, cause);
        }
    };

  public:
    /// Everything the server keeps: about 8 KiB, most of it the two message
    /// buffers, so main() builds it outside its stack.
    struct Storage {
        std::array<Client, MAX_CLIENTS> clients;
        /// Where a client's on_signals() reads the one message it handles, and
        /// where it builds the reply. One of each serves every client, since
        /// one on_signals() runs at a time.
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        std::array<u8, wire::MAX_MESSAGE_BYTES> reply_bytes;
    };

    /// A server with no client, keeping its state in |storage|, waiting on
    /// |loop| and answering from |tables|. All three must outlive it.
    RouteServer(Storage &storage, loop::Loop &loop, acpi_route::TableDirectory const &tables)
        : m_storage(&storage), m_loop(&loop), m_tables(&tables) {}

    RouteServer(RouteServer const &) = delete;
    auto operator=(RouteServer const &) -> RouteServer & = delete;

  private:
    /// Takes |channel|, a new client's, into a free entry and a slot; closes it
    /// with an epitaph when there is neither. ioservice::Outgoing hands an Open
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

    /// Frees |client|'s entry; its slot closes the channel, by answering Close.
    static auto release(Client &client) -> loop::Action {
        client.channel = Uandle::Invalid;
        return loop::Action::Close;
    }

    /// Frees |client|'s entry after the Loop failed to arm its wait again. The
    /// Loop has closed the channel and freed the slot, so the entry is all that
    /// is left to free.
    auto rearm_failed(Client &client, Status const cause) -> void {
        client.channel = Uandle::Invalid;
        report("cannot wait on a client again", cause);
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
        // <ours_acpi/ours_acpi.hpp> lays down. Neither method takes a handle,
        // so any the message brought are closed with it.
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        if (header.is_ok() && header->ordinal == ours_acpi::ISA_IRQ.ordinal) {
            if (auto const request = ours_acpi::decode_isa_irq(message); request.is_ok()) {
                return isa_irq(client, *request);
            }
        } else if (header.is_ok() && header->ordinal == ours_acpi::ECAM_BASE.ordinal) {
            if (auto const request = ours_acpi::decode_ecam_base(message); request.is_ok()) {
                return ecam_base(client, *request);
            }
        }
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
        return release(client);
    }

    /// IsaIrq: the GSI of the ISA line, the interrupt domain that GSI is a
    /// number of, and how it signals, from the MADT's overrides, or why there
    /// is none.
    auto isa_irq(Client &client, ours_acpi::IsaIrqRequest const &request) -> loop::Action {
        auto reply = ours_acpi::IsaIrqReply{.status = Status::Ok};
        if (auto const route = acpi_route::isa_irq_to_gsi(*m_tables, request.isa_irq);
            route.is_ok()) {
            // ACPI's numbering is the root controller's: the GSI is a number
            // of the root domain, whose name ours/interrupt.hpp keeps.
            reply.domain = ours::ROOT_INTERRUPT_DOMAIN;
            reply.gsi = route->gsi;
            reply.trigger = wire_trigger(route->trigger);
            reply.polarity = wire_polarity(route->polarity);
        } else {
            reply = {.status = route.unwrap_err()};
        }
        auto const encoded =
            ours_acpi::encode_isa_irq_reply(m_storage->reply_bytes, request.txid, reply);
        return answer(client, encoded);
    }

    /// EcamBase: where the segment's configuration space is, from the MCFG, or
    /// why there is none.
    auto ecam_base(Client &client, ours_acpi::EcamBaseRequest const &request) -> loop::Action {
        auto reply = ours_acpi::EcamBaseReply{.status = Status::Ok};
        if (auto const window = acpi_route::ecam_window(*m_tables, request.segment);
            window.is_ok()) {
            reply.address = window->address;
            reply.size = window->size;
            reply.first_bus = window->first_bus;
            reply.last_bus = window->last_bus;
        } else {
            reply = {.status = window.unwrap_err()};
        }
        auto const encoded =
            ours_acpi::encode_ecam_base_reply(m_storage->reply_bytes, request.txid, reply);
        return answer(client, encoded);
    }

    /// Writes |encoded|, a reply, to |client|. A reply that cannot be built or
    /// written leaves its client waiting for good, so the connection goes with
    /// it.
    auto answer(Client &client, ustl::Result<wire::Message, Status> const &encoded)
        -> loop::Action {
        if (encoded.is_err()) {
            report("cannot encode a reply", encoded.unwrap_err());
            return release(client);
        }
        if (auto const written = write(client.channel, encoded.unwrap()); written != Status::Ok) {
            return release(client);
        }
        return loop::Action::Rearm;
    }

    Storage *m_storage;
    loop::Loop *m_loop;
    acpi_route::TableDirectory const *m_tables;
};

// What the server keeps, outside the stack: constinit, because nothing runs a
// user program's dynamic initialisers. The outgoing directory's storage holds
// types with destructors (ustl::Option, in the ioservice connections), which as
// a plain global would register them with __cxa_atexit, which the runtime does
// not have; ustl::LazyInit is raw storage that main() builds it in instead, as
// init's are (system/service/init/init.cpp).
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<RouteServer::Storage> ROUTE_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};

/// The tables, mapped read only into this process from the VMO ours.boot.Acpi
/// hands out and checked: the bytes stay mapped for as long as the program
/// lives, which is what TableDirectory asks of them. Otherwise why not.
auto map_tables(Uandle const vma) -> ustl::Result<acpi_route::TableDirectory, Status> {
    auto const vmo = ours_resource::get(ours_resource::ACPI_PROTOCOL);
    if (vmo.is_err()) {
        return ustl::err(vmo.unwrap_err());
    }
    auto size = u64{0};
    if (auto const status = ours::vmo_get_size(vmo.unwrap(), &size); status != Status::Ok) {
        return ustl::err(status);
    }
    auto at = ours::VirtAddr{0};
    if (auto const status =
            ours::vma_map(vma, ours::VmOptions::PermRead, 0, vmo.unwrap(), 0, size, &at);
        status != Status::Ok) {
        return ustl::err(status);
    }
    return acpi_route::TableDirectory::from_bytes(
        {reinterpret_cast<u8 const *>(at), static_cast<usize>(size)});
}
} // namespace

auto main(int, char **) -> int {
    // The server end of the outgoing directory, which the root made when it
    // resolved this component and hands every component with a program.
    auto const directory_request =
        driver_kit::startup_handle(ours::HandleType::DirectoryRequest, "a DirectoryRequest handle");
    if (directory_request.is_err()) {
        return 1;
    }
    auto const vma = driver_kit::startup_handle(ours::HandleType::VmaRoot, "VmaRoot");
    if (vma.is_err()) {
        return 1;
    }

    // Without tables there is nothing to answer from.
    auto const tables = map_tables(vma.unwrap());
    if (tables.is_err()) {
        report("no usable ACPI tables", tables.unwrap_err());
        return 1;
    }
    char count[10];
    auto _ = ort::debug_line(
        {"acpi-route: serving, from ", ort::decimal(tables->count(), count), " ACPI tables"});

    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("cannot make its port", status);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);
    RouteServer server(*ROUTE_STORAGE.init(), loop, tables.unwrap());

    if (auto const status = outgoing.add_protocol(ours_acpi::ROUTE_NAME, server);
        status != Status::Ok) {
        report("cannot add its protocol", status);
        return 1;
    }
    if (auto const status = outgoing.serve(directory_request.unwrap()); status != Status::Ok) {
        report("cannot serve its outgoing directory", status);
        auto _ = ours::handle_close(directory_request.unwrap());
        return 1;
    }

    // run() comes back only when the port itself fails.
    report("its Loop stopped", loop.run(ours::INSTANT_INFINITE));
    return 1;
}
