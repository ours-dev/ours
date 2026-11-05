/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROT_RESOURCE_CLIENT_HPP
#define PROT_RESOURCE_CLIENT_HPP 1

#include <prot/resource.hpp>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

/// The client side of the ours.resource protocols, for user programs: how a
/// component that a route reaches a resource protocol gets the resource,
/// from a path in its namespace rather than a startup handle.
namespace prot::resource {
/// The resource |protocol| hands out, from /svc/<its name> in this process's
/// namespace: one Get on a connection of its own, closed after it. The
/// resource is the caller's. Otherwise why not: the connect's status
/// (NotFound when no use of the protocol is in the namespace), the epitaph a
/// route that leads nowhere closed the connection with, the reply's status
/// (a root that does not hold the resource says so), PeerClosed for a
/// connection closed without one, InvArgs for an answer that is not a
/// well-formed reply, or a channel call's own.
[[nodiscard]]
auto get(Protocol const &protocol) -> ustl::Result<ours::Uandle, ours::Status>;

/// The same Get on |connection|, a new connection to |protocol| that the
/// caller keeps.
[[nodiscard]]
auto get(ours::Uandle connection, Protocol const &protocol)
    -> ustl::Result<ours::Uandle, ours::Status>;
} // namespace prot::resource

#endif // PROT_RESOURCE_CLIENT_HPP
