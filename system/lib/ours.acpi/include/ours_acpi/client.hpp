/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ACPI_CLIENT_HPP
#define OURS_ACPI_CLIENT_HPP 1

#include <ours_acpi/ours_acpi.hpp>

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
namespace ours_acpi {
/// The GSI of ISA interrupt |isa_irq| and how it signals (Route.IsaIrq).
[[nodiscard]]
auto isa_irq(u32 isa_irq) -> ustl::Result<IsaIrqReply, ours::Status>;

/// Where PCI segment |segment|'s configuration space is (Route.EcamBase).
[[nodiscard]]
auto ecam_base(u16 segment) -> ustl::Result<EcamBaseReply, ours::Status>;
} // namespace ours_acpi

#endif // #ifndef OURS_ACPI_CLIENT_HPP
