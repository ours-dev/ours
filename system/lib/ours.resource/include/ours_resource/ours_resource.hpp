/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_RESOURCE_OURS_RESOURCE_HPP
#define OURS_RESOURCE_OURS_RESOURCE_HPP 1

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// ours.resource, the protocols through which the root hands out kernel
/// resources, written by hand on <wire/wire.hpp> (component-framework design
/// S6, D19, D20, "The protocol subset"; .agents/tasks/microkernel-interrupts/
/// DESIGN.md, D5). Each protocol is named after the resource it hands out and
/// has one method, Get, whose reply carries a handle to that resource:
///
///   Debug    the System Debug resource, which debug_read takes
///   Irq      the root of the interrupt lines, which interrupt_create takes
///            for a physical line
///   Ioport   the root of the I/O ports, which ioports_request takes
///   Mmio     the root of the physical address space, the capability to
///            map device registers; the kernel's own memory and devices are
///            outside it
///
/// They are built-ins of the root, and the only way to these resources: no
/// component gets one as a startup handle. Routing is the only gate: the
/// root answers every Get that reaches it without asking who sent it, so
/// what keeps a resource from a component is that no manifest offers it the
/// protocol.
///
/// Each ordinal is hashed from its selector, such as "ours.resource/Irq.Get"
/// (wire::Method). The Gets have one shape, flat, at fixed offsets from
/// the start of the message:
///
///   Get   two-way  (nothing)
///
/// and its Ok reply, the status at 16 as in every reply:
///
///   Get   u32 resource@20 (handle)
///
/// The reply puts a status in front of the resource, as every reply here
/// has one, so a root that cannot duplicate the resource answers with the
/// 24-byte status form rather than dropping the connection.
///
/// Each protocol is closed: a message with any other ordinal -- another
/// protocol's Get among them -- closes the connection, as does one its
/// decoder refuses (InvArgs), without an epitaph.
namespace ours_resource {
/// One protocol of the library: the name a component finds it under in its
/// /svc, the library and the protocol joined by a dot, and its one method.
struct Protocol {
    std::string_view name;
    /// Get, which asks for a handle to the resource the protocol is named
    /// after.
    wire::Method get;
};

// The protocols, each Get declared beside the selector its ordinal is hashed
// from. wire/test/ordinals_test.py recomputes every ordinal from its
// selector.

/// ours.resource.Debug, which hands out the System Debug resource.
inline constexpr auto DEBUG_PROTOCOL = Protocol{
    .name = "ours.resource.Debug",
    .get =
        wire::Method{
            .selector = "ours.resource/Debug.Get",
            .ordinal = 0x226c'd5bb'1821'8fda,
            .kind = wire::MethodKind::TwoWay,
        },
};

/// ours.resource.Irq, which hands out the root of the interrupt lines: an
/// Irq resource of base 0 and size 0, which covers every line.
inline constexpr auto IRQ_PROTOCOL = Protocol{
    .name = "ours.resource.Irq",
    .get =
        wire::Method{
            .selector = "ours.resource/Irq.Get",
            .ordinal = 0x273c'3e02'af63'50e0,
            .kind = wire::MethodKind::TwoWay,
        },
};

/// ours.resource.Ioport, which hands out the root of the I/O ports: an IoPort
/// resource of base 0 and size 0, which covers every port, though no resource
/// grants the ports the kernel drives itself.
inline constexpr auto IOPORT_PROTOCOL = Protocol{
    .name = "ours.resource.Ioport",
    .get =
        wire::Method{
            .selector = "ours.resource/Ioport.Get",
            .ordinal = 0x231b'c3b7'4044'47b5,
            .kind = wire::MethodKind::TwoWay,
        },
};

/// ours.resource.Mmio, which hands out the root of the physical address space:
/// an MmIo resource of base 0 and size 0, which covers every address, though no
/// resource grants the RAM or the devices the kernel owns.
inline constexpr auto MMIO_PROTOCOL = Protocol{
    .name = "ours.resource.Mmio",
    .get =
        wire::Method{
            .selector = "ours.resource/Mmio.Get",
            .ordinal = 0x1f6f'1386'9360'0a40,
            .kind = wire::MethodKind::TwoWay,
        },
};

/// ours.resource.Sampling, which hands out the System Sampling resource, the
/// capability sampler_create asks for.
inline constexpr auto SAMPLING_PROTOCOL = Protocol{
    .name = "ours.resource.Sampling",
    .get =
        wire::Method{
            .selector = "ours.resource/Sampling.Get",
            .ordinal = 0x62a8'8580'9507'5fb4,
            .kind = wire::MethodKind::TwoWay,
        },
};

/// ours.kernel.RootJob, which hands out the root job with every right its
/// holder has: what a system-wide profiler or debugger enumerates processes
/// under (object_get_info JobProcesses, object_get_child) and binds a
/// debugger exception channel to. Its library is ours.kernel, but it has the
/// same shape as this library's protocols, one Get answered with a handle,
/// so it is declared and coded here.
inline constexpr auto ROOT_JOB_PROTOCOL = Protocol{
    .name = "ours.kernel.RootJob",
    .get =
        wire::Method{
            .selector = "ours.kernel/RootJob.Get",
            .ordinal = 0x765b'31d7'e30c'32b5,
            .kind = wire::MethodKind::TwoWay,
        },
};

struct GetRequest {
    u32 txid;
};

/// The reply. |resource| is there when, and only when, |status| is Ok, so a
/// failure is written {.status = ...}.
struct GetReply {
    ours::Status status;
    ours::Uandle resource = ours::Uandle::Invalid;
};

/// The Get of |protocol|, encoded by a client into |bytes| and decoded by the
/// server. The encoder refuses a txid outside 1 to MAX_TXID (InvArgs) and a
/// buffer too short (BufferTooSmall); the decoder refuses a malformed
/// request, and the Get of any other protocol, with InvArgs.
[[nodiscard]]
auto encode_get(std::span<u8> bytes, Protocol const &protocol, GetRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_get(wire::Message message, Protocol const &protocol)
    -> ustl::Result<GetRequest, ours::Status>;

/// The reply to the Get |txid| of |protocol|, encoded by the server and
/// decoded by the client. A reply whose status is not Ok is the status form:
/// the encoder refuses a resource next to a failure (InvArgs). A decoder's
/// Err means the reply is malformed, or answers another protocol's Get; the
/// server's own failure is in the reply's |status|.
[[nodiscard]]
auto encode_get_reply(std::span<u8> bytes, std::span<ours::Uandle> handles,
                      Protocol const &protocol, u32 txid, GetReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_get_reply(wire::Message message, Protocol const &protocol, u32 txid)
    -> ustl::Result<GetReply, ours::Status>;
} // namespace ours_resource

#endif // #ifndef OURS_RESOURCE_OURS_RESOURCE_HPP
