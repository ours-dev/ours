/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROT_ACPI_CLIENT_HPP
#define PROT_ACPI_CLIENT_HPP 1

#include <prot/acpi.hpp>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

/// The client side of ours.acpi.Route, for user programs: how a component
/// that a route reaches the route service asks it a question, from a path in
/// its namespace.
///
/// Each call is one request on a connection of its own, closed after the
/// answer. Otherwise why not: the connect's status (NotFound when no use of the
/// protocol is in the namespace), the epitaph a route that leads nowhere closed
/// the connection with (NotFound again when the service has exited, and
/// NoResource when it has no room for another client), the service's own
/// status for the question (NotFound for a line or a segment the tables do not
/// have, InvArgs for tables that cannot be read to the answer),
/// PeerClosed for a connection closed without a reply, InvArgs for an answer
/// that is not a well-formed reply, or a channel call's own.
namespace prot::acpi {
/// The GSI of ISA interrupt |isa_irq| and how it signals (Route.IsaIrq), on
/// |connection|, a connection to the route service the caller keeps: one
/// request on it, and the connection is the caller's to close or ask
/// again -- the caller opens it (fsio::connect on ROUTE_NAME, as for any
/// protocol) rather than this call.
[[nodiscard]]
auto isa_irq(ours::Uandle connection, u32 isa_irq) -> ustl::Result<IsaIrqReply, ours::Status>;

/// Where PCI segment |segment|'s configuration space is (Route.EcamBase), on
/// |connection|, as isa_irq() above.
[[nodiscard]]
auto ecam_base(ours::Uandle connection, u16 segment) -> ustl::Result<EcamBaseReply, ours::Status>;

/// The GSI of ISA interrupt |isa_irq| and how it signals (Route.IsaIrq): one
/// request on a connection of its own to /svc/ours.acpi.Route, closed after
/// the answer.
[[nodiscard]]
auto isa_irq(u32 isa_irq) -> ustl::Result<IsaIrqReply, ours::Status>;

/// Where PCI segment |segment|'s configuration space is (Route.EcamBase): one
/// request on a connection of its own to /svc/ours.acpi.Route, closed after
/// the answer.
[[nodiscard]]
auto ecam_base(u16 segment) -> ustl::Result<EcamBaseReply, ours::Status>;
} // namespace prot::acpi

#endif // #ifndef PROT_ACPI_CLIENT_HPP
