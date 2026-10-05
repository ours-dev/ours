/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// pc-ps2 -- the PS/2 keyboard driver, a component of dev's root
/// (.agents/tasks/microkernel-interrupts/PLAN.md, step 12). It reads the
/// keyboard from user space: the kernel only hands it the interrupt line and
/// the I/O ports, and knows nothing of keys.
///
/// The objects:
///   - the roots of the interrupt lines and of the I/O ports, which the root
///     hands out through its built-ins ours.resource.Irq and
///     ours.resource.Ioport, reached through this component's /svc because
///     root.cm offers it both;
///   - the i8042's two ports, 0x60 and 0x64, opened to this process with
///     ioports_request (i8042.hpp);
///   - an interrupt object on ISA 1, the keyboard's line, made with RemapIrq
///     in the default mode, which keeps the line as the platform programmed
///     it: edge-triggered, as an ISA line is unless the firmware's tables say
///     otherwise. So the kernel does not mask it from a packet to its ack,
///     and interrupts in between only fold into the next packet;
///   - a port the interrupt is bound to, on which the one thread waits;
///   - the decoder, which turns the bytes into keys (keymap.hpp).
///
/// The events, for one key:
///   1. the keyboard sends its scancode; the controller holds it in its
///      output buffer and raises ISA 1;
///   2. the kernel's handler on the line queues the interrupt object's one
///      packet on the port, and wakes this thread in port_wait;
///   3. the driver reads the data port while the status says a byte waits,
///      and logs each key pressed, as `pc-ps2: key a`;
///   4. interrupt_ack re-arms the object. A byte that came after the last
///      read raised the line in between, so the ack sends the next packet
///      at once, and no byte waits unread for an interrupt that already
///      came.
///
/// Before it says `pc-ps2: ready` it checks what hands the line back when a
/// driver dies: while an interrupt object holds the line, a second create
/// answers AlreadyBound, and once the holder's handle closes, as every
/// handle of a dying process does, a create takes the line again. A check
/// that fails says so with FAILED in the line, which fails ours-verify's
/// boot check.
#include <ours/interrupt.hpp>
#include <ours/port.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>
#include <string_view>

#include "i8042.hpp"
#include "keymap.hpp"
#include <fsio/fsio.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ours_resource/ours_resource.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// The protocols its manifest uses, each at /svc/<name>.
constexpr auto IRQ_RESOURCE = std::string_view{"/svc/ours.resource.Irq"};
constexpr auto IOPORT_RESOURCE = std::string_view{"/svc/ours.resource.Ioport"};

/// The keyboard's ISA IRQ, which RemapIrq turns into the line it arrives on.
constexpr auto ISA_KEYBOARD = u32{1};

/// The key its interrupt's packets carry. Nothing else is bound to the
/// port, so any value would do.
constexpr auto KEYBOARD_KEY = u64{1};

/// Says on the debug console that |what| answered |status|.
auto report(std::string_view const what, Status const status) -> void {
    auto _ = ort::debug_line({"pc-ps2: ", what, ": ", ours::to_string(status)});
}

/// Says that the check |what| answered |got| where |expected| was due, with
/// FAILED in the line.
auto fail(std::string_view const what, Status const got, Status const expected) -> void {
    auto _ = ort::debug_line({"pc-ps2: ", what, " = ", ours::to_string(got), " (expected ",
                              ours::to_string(expected), ") FAILED"});
}

/// Get on |connection|, a new connection to |protocol|: the resource, which
/// the caller owns, or why not -- the reply's status, the epitaph a failed
/// route closed the connection with, PeerClosed when it closed without one,
/// or InvArgs for an answer that is not a well-formed reply.
auto get(Uandle const connection, ours_resource::Protocol const &protocol)
    -> ustl::Result<Uandle, Status> {
    // The one request on this connection, so any txid will do.
    constexpr auto TXID = u32{1};
    std::array<u8, wire::STATUS_FORM_SIZE> request_bytes{};
    auto const request = ours_resource::encode_get(request_bytes, protocol, {.txid = TXID});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    // A write that finds the connection closed is not the answer: the
    // epitaph the root closed it with is, and it is still there to read.
    if (auto const written =
            ours::channel_write(connection, ours::ChannelWriteOptions::None, request->bytes.data(),
                                static_cast<u32>(request->bytes.size()), nullptr, 0);
        written != Status::Ok && written != Status::PeerClosed) {
        return ustl::err(written);
    }
    auto observed = ours::Signals::None;
    if (auto const status =
            ours::object_wait_one(connection, ours::Signals::Readable | ours::Signals::PeerClosed,
                                  ours::INSTANT_INFINITE, &observed);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (!(observed & ours::Signals::Readable)) {
        return ustl::err(Status::PeerClosed);
    }

    // Either answer, an epitaph or the reply, is the 24-byte status form or
    // as long, with at most the resource in it.
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    std::array<Uandle, 1> handles{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const status = ours::channel_read(connection, ours::ChannelReadOptions::None,
                                               bytes.data(), handles.data(), bytes.size(),
                                               handles.size(), &num_bytes, &num_handles);
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto const message = wire::Message{
        .bytes = std::span{bytes}.first(num_bytes),
        .handles = std::span{handles}.first(num_handles),
    };
    auto const close_handles = [&message] {
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
    };
    if (auto const header = wire::decode_header(message.bytes, wire::Direction::ToClient);
        header.is_ok() && wire::is_epitaph(header.unwrap())) {
        close_handles();
        auto const epitaph = wire::decode_epitaph(message);
        if (epitaph.is_err()) {
            return ustl::err(Status::InvArgs);
        }
        // An Ok epitaph says nothing about why; it reads as the connection
        // closing.
        if (epitaph.unwrap() == Status::Ok) {
            return ustl::err(Status::PeerClosed);
        }
        return ustl::err(epitaph.unwrap());
    }
    auto const decoded = ours_resource::decode_get_reply(message, protocol, TXID);
    if (decoded.is_err()) {
        close_handles();
        return ustl::err(Status::InvArgs);
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    return ustl::ok(decoded->resource);
}

/// The resource |protocol| hands out, from the protocol at |path| in /svc:
/// one Get on a connection of its own, closed after it.
auto get_resource(std::string_view const path, ours_resource::Protocol const &protocol)
    -> ustl::Result<Uandle, Status> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const status = fsio::connect(path, server); status != Status::Ok) {
        auto _ = ours::handle_close(client);
        return ustl::err(status);
    }
    auto const resource = get(client, protocol);
    auto _ = ours::handle_close(client);
    return resource;
}

/// Opens the i8042's two ports to this process with |ioport|, the root of
/// the I/O ports: each port alone, since 0x61 to 0x63 between them belong
/// to other devices.
auto open_ports(Uandle const ioport) -> Status {
    for (auto const port: {pc_ps2::i8042::COMMAND_PORT, pc_ps2::i8042::DATA_PORT}) {
        if (auto const status = ours::ioports_request(ioport, port, 1); status != Status::Ok) {
            return status;
        }
    }
    return Status::Ok;
}

/// An interrupt object on the keyboard's line, made with |irq|, the root of
/// the interrupt lines, after the check of what hands the line back that
/// the file's comment describes. The check's failures are FAILED lines;
/// the line is taken all the same where it can be.
auto take_line(Uandle const irq) -> ustl::Result<Uandle, Status> {
    constexpr auto OPTIONS = ours::InterruptOptions::RemapIrq;
    auto first = Uandle::Invalid;
    if (auto const status = ours::interrupt_create(irq, ISA_KEYBOARD, OPTIONS, &first);
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto second = Uandle::Invalid;
    if (auto const status = ours::interrupt_create(irq, ISA_KEYBOARD, OPTIONS, &second);
        status != Status::AlreadyBound) {
        fail("interrupt_create(ISA 1, held)", status, Status::AlreadyBound);
        if (status == Status::Ok) {
            auto _ = ours::handle_close(second);
        }
    }
    if (auto const status = ours::handle_close(first); status != Status::Ok) {
        return ustl::err(status);
    }
    auto line = Uandle::Invalid;
    if (auto const status = ours::interrupt_create(irq, ISA_KEYBOARD, OPTIONS, &line);
        status != Status::Ok) {
        fail("interrupt_create(ISA 1, after its holder closed)", status, Status::Ok);
        return ustl::err(status);
    }
    return ustl::ok(line);
}

/// Takes every byte the controller holds, and logs the keys they press.
auto read_keys(pc_ps2::Decoder &decoder) -> void {
    for (auto byte = pc_ps2::i8042::read_byte(); byte.is_some();
         byte = pc_ps2::i8042::read_byte()) {
        if (auto const key = decoder.feed(*byte); key.is_some()) {
            auto _ = ort::debug_line({"pc-ps2: key ", key->name()});
        }
    }
}

/// Waits on |port| for |interrupt|'s packets, reads the keyboard on each,
/// then acks. Returns only when a wait or an ack fails, with why.
auto serve(Uandle const port, Uandle const interrupt, pc_ps2::Decoder &decoder) -> Status {
    for (;;) {
        ours::PortPacket packet{};
        if (auto const status = ours::port_wait(port, ours::INSTANT_INFINITE, &packet);
            status != Status::Ok) {
            report("port_wait", status);
            return status;
        }
        if (packet.type != ours::PacketType::Interrupt || packet.key != KEYBOARD_KEY) {
            continue;
        }
        read_keys(decoder);
        if (auto const status = ours::interrupt_ack(interrupt); status != Status::Ok) {
            report("interrupt_ack", status);
            return status;
        }
    }
}
} // namespace

auto main(int, char **) -> int {
    if (auto const status = fsio::install(); status != Status::Ok) {
        report("fsio::install()", status);
        return 1;
    }

    // The ports first, which the reset needs, then the line, which is
    // taken once the controller raises it only for keys.
    auto const ioport = get_resource(IOPORT_RESOURCE, ours_resource::IOPORT_PROTOCOL);
    if (ioport.is_err()) {
        report("getting the I/O port resource", ioport.unwrap_err());
        return 1;
    }
    if (auto const status = open_ports(ioport.unwrap()); status != Status::Ok) {
        report("ioports_request", status);
        return 1;
    }
    // The ports stay open without it. A Get's duplicate is charged to the
    // root until it is closed, so a driver keeps none it no longer needs.
    auto _ = ours::handle_close(ioport.unwrap());

    if (auto const status = pc_ps2::i8042::reset(); status != Status::Ok) {
        report("resetting the keyboard controller", status);
        return 1;
    }

    auto const irq = get_resource(IRQ_RESOURCE, ours_resource::IRQ_PROTOCOL);
    if (irq.is_err()) {
        report("getting the IRQ resource", irq.unwrap_err());
        return 1;
    }
    auto const interrupt = take_line(irq.unwrap());
    // The interrupt object holds the line for as long as it lives, without
    // the resource.
    auto _ = ours::handle_close(irq.unwrap());
    if (interrupt.is_err()) {
        report("taking ISA 1", interrupt.unwrap_err());
        return 1;
    }

    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::BindToInterrupt, &port);
        status != Status::Ok) {
        report("port_create", status);
        return 1;
    }
    if (auto const status = ours::interrupt_bind(interrupt.unwrap(), port, KEYBOARD_KEY,
                                                 ours::InterruptBindOptions::Bind);
        status != Status::Ok) {
        report("interrupt_bind", status);
        return 1;
    }

    // A key pressed before the line was taken raised it while nobody held
    // it, and the edge is gone: its bytes would wait in the controller
    // forever, and keep any later key from raising the line. They are read
    // now, with the line held; a byte that comes meanwhile sends a packet.
    pc_ps2::Decoder decoder;
    read_keys(decoder);
    auto _ = ort::debug_line("pc-ps2: ready");

    // It returns only when a wait or an ack has failed, and has said so.
    auto _ = serve(port, interrupt.unwrap(), decoder);
    return 1;
}
