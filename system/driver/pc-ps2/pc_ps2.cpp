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
///   - the ACPI route service, ours.acpi.Route, which root.cm offers it too:
///     the keyboard is ISA IRQ 1 to this driver, and the service says which
///     global system interrupt (GSI) that is on this machine, from the
///     firmware's tables. The kernel is given that GSI and never the ISA
///     number;
///   - the i8042's two ports, 0x60 and 0x64, opened to this process with
///     ioports_request (i8042.hpp);
///   - an interrupt object on the keyboard's line, whose number interrupt_map
///     gives for that GSI, made in the default mode, which keeps the line as
///     the platform programmed it: edge-triggered, as an ISA line is unless
///     the firmware's tables say otherwise. On an edge line the kernel does
///     not mask it from a packet to its ack, and interrupts in between only
///     fold into the next packet; on a level line it masks it until the ack,
///     which the loop handles too (it drains the controller, then acks);
///   - a port the interrupt is bound to, on which the one thread waits;
///   - the decoder, which turns the bytes into keys (keymap.hpp).
///
/// The events, for one key:
///   1. the keyboard sends its scancode; the controller holds it in its
///      output buffer and raises its line (ISA 1);
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
#include <prot/acpi.hpp>
#include <prot/acpi_client.hpp>
#include <prot/resource.hpp>
#include <prot/resource_client.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// The protocols its manifest uses, each at /svc/<name>: ours.resource.Irq,
/// ours.resource.Ioport and ours.acpi.Route.

/// The keyboard's ISA IRQ, the one number a PC's keyboard is known by. Only
/// the route service is asked what it means on this machine; the kernel is
/// never given it.
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

/// How a line signals, as the route service says it, for the log.
auto trigger_name(prot::acpi::Trigger const trigger) -> std::string_view {
    switch (trigger) {
        case prot::acpi::Trigger::Edge:
            return "edge";
        case prot::acpi::Trigger::Level:
            return "level";
    }
    return "unknown trigger";
}

auto polarity_name(prot::acpi::Polarity const polarity) -> std::string_view {
    switch (polarity) {
        case prot::acpi::Polarity::ActiveHigh:
            return "active high";
        case prot::acpi::Polarity::ActiveLow:
            return "active low";
    }
    return "unknown polarity";
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

/// What the route service says of the keyboard's line: the GSI it arrives on,
/// which the firmware's interrupt source overrides decide and which need not
/// equal the ISA number, and how it signals. Logged in one line. The error is
/// why the service could not say (see prot/acpi_client.hpp): not reached,
/// gone, or no such line in the tables.
auto keyboard_route() -> ustl::Result<prot::acpi::IsaIrqReply, Status> {
    auto const route = prot::acpi::isa_irq(ISA_KEYBOARD);
    if (route.is_err()) {
        return ustl::err(route.unwrap_err());
    }
    char isa[10];
    char gsi[10];
    auto _ = ort::debug_line({"pc-ps2: ISA ", ort::decimal(ISA_KEYBOARD, isa), " is on GSI ",
                              ort::decimal(route->gsi, gsi), ", ", trigger_name(route->trigger),
                              ", ", polarity_name(route->polarity)});
    return ustl::ok(route.unwrap());
}

/// An interrupt object on the keyboard's line, which is the pin |gsi| in
/// interrupt domain |domain|, made with |irq|, the root of the interrupt
/// lines, after the check of what hands the line back that the file's comment
/// describes. The check's failures are FAILED lines; the line is taken all
/// the same where it can be.
auto take_line(Uandle const irq, u32 const domain, u32 const gsi) -> ustl::Result<Uandle, Status> {
    // The line is named by the kernel's number for it, and the pair the route
    // service answered -- the domain the GSI is a number of, and the GSI --
    // is what interrupt_map resolves it from.
    auto number = u32{0};
    if (auto const status = ours::interrupt_map(irq, domain, gsi, &number); status != Status::Ok) {
        report("interrupt_map(the keyboard's GSI)", status);
        return ustl::err(status);
    }
    // The default mode, though the route service also said how the line
    // signals: the kernel programmed every ISA pin from the same tables'
    // interrupt source overrides when it started, by the same rules (a later
    // override of a number replaces an earlier, the entry's bus is not read),
    // so the pin already signals as the route says, and a mode given here
    // would only write the same values again.
    constexpr auto OPTIONS = ours::InterruptOptions::ModeDefault;
    auto first = Uandle::Invalid;
    if (auto const status = ours::interrupt_create(irq, number, OPTIONS, &first);
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto second = Uandle::Invalid;
    if (auto const status = ours::interrupt_create(irq, number, OPTIONS, &second);
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
    if (auto const status = ours::interrupt_create(irq, number, OPTIONS, &line);
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

    // Where the keyboard's line is comes first: a driver that cannot say
    // stops before it touches the controller, and holds no resource of the
    // root while it waits for the service.
    auto const route = keyboard_route();
    if (route.is_err()) {
        report("asking the route service for the keyboard's GSI", route.unwrap_err());
        return 1;
    }

    // The ports next, which the reset needs, then the line, which is
    // taken once the controller raises it only for keys.
    auto const ioport = prot::resource::get(prot::resource::IOPORT_PROTOCOL);
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

    auto const irq = prot::resource::get(prot::resource::IRQ_PROTOCOL);
    if (irq.is_err()) {
        report("getting the IRQ resource", irq.unwrap_err());
        return 1;
    }
    auto const interrupt = take_line(irq.unwrap(), route->domain, route->gsi);
    // The interrupt object holds the line for as long as it lives, without
    // the resource.
    auto _ = ours::handle_close(irq.unwrap());
    if (interrupt.is_err()) {
        report("taking the keyboard's line", interrupt.unwrap_err());
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
