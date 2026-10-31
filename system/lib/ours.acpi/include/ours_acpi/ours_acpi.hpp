/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ACPI_OURS_ACPI_HPP
#define OURS_ACPI_OURS_ACPI_HPP 1

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// ours.acpi, hand-written on //system/lib/wire: the protocol of the ACPI
/// route service, which answers from the firmware's static tables where an
/// interrupt arrives and where the PCI configuration space is. A caller asks
/// it instead of knowing a GSI or an ECAM base itself.
///
/// Route has two two-way methods:
///
///   Route.IsaIrq    the GSI an ISA interrupt arrives on, and how it signals
///   Route.EcamBase  where one PCI segment's configuration space is
///
/// Their bodies are flat, at fixed offsets from the start of the message:
///
///   Route.IsaIrq    u32 isa_irq@16
///   Route.EcamBase  u32 segment@16, the upper half zero
///
/// and their Ok replies, the status at 16 as in every reply:
///
///   Route.IsaIrq    u32 gsi@20, u32 trigger@24, u32 polarity@28            32 bytes
///   Route.EcamBase  u8 first_bus@20, u8 last_bus@21, zero u16@22,
///                   u64 address@24, u64 size@32                            40 bytes
///
/// A reply that is not Ok is the status form alone, with no body: NotFound
/// says the tables have no such line or segment, InvArgs that they cannot be
/// read to the answer.
///
/// Route is a closed protocol: a message with any other ordinal closes the
/// connection, as does one its decoder refuses (InvArgs), without an epitaph.
namespace ours_acpi {
/// The name a component finds the protocol under in its /svc, and the one the
/// route service serves it under in its outgoing directory's svc/.
inline constexpr auto ROUTE_NAME = std::string_view{"ours.acpi.Route"};

// The methods, each declared beside the selector its ordinal is hashed from.
// wire/test/ordinals_test.py recomputes the ordinals from the selectors.

/// The GSI of an ISA interrupt (Route.IsaIrq).
inline constexpr auto ISA_IRQ = wire::Method{
    .selector = "ours.acpi/Route.IsaIrq",
    .ordinal = 0x1785'1769'243e'80ff,
    .kind = wire::MethodKind::TwoWay,
};

/// The configuration space of a PCI segment (Route.EcamBase).
inline constexpr auto ECAM_BASE = wire::Method{
    .selector = "ours.acpi/Route.EcamBase",
    .ordinal = 0x64f7'3165'540e'cd7d,
    .kind = wire::MethodKind::TwoWay,
};

/// How an interrupt signals. The values are ABI and never change.
enum class Trigger : u8 {
    Edge = 0,
    Level = 1,
};

enum class Polarity : u8 {
    ActiveHigh = 0,
    ActiveLow = 1,
};

struct IsaIrqRequest {
    u32 txid;
    u32 isa_irq;
};

/// The reply. The rest is there only when |status| is Ok, so a failure is
/// written {.status = ...}.
struct IsaIrqReply {
    ours::Status status;
    u32 gsi = 0;
    Trigger trigger = Trigger::Edge;
    Polarity polarity = Polarity::ActiveHigh;
};

struct EcamBaseRequest {
    u32 txid;
    u16 segment;
};

/// The reply. The rest is there only when |status| is Ok. |address| is where
/// the configuration space of |first_bus| begins and |size| the bytes of it up
/// to the end of |last_bus|, one MiB to a bus.
struct EcamBaseReply {
    ours::Status status;
    u64 address = 0;
    u64 size = 0;
    u8 first_bus = 0;
    u8 last_bus = 0;
};

/// Each request, encoded by a client into |bytes| and decoded by the server.
/// The encoders refuse a txid outside 1 to MAX_TXID (InvArgs) and a buffer too
/// short (BufferTooSmall). The decoders refuse a malformed request with
/// InvArgs.
[[nodiscard]]
auto encode_isa_irq(std::span<u8> bytes, IsaIrqRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_isa_irq(wire::Message message) -> ustl::Result<IsaIrqRequest, ours::Status>;

[[nodiscard]]
auto encode_ecam_base(std::span<u8> bytes, EcamBaseRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_ecam_base(wire::Message message) -> ustl::Result<EcamBaseRequest, ours::Status>;

/// The replies to the request |txid|, encoded by the server and decoded by the
/// client. An encoder refuses a body next to a failure, and a value its enums
/// do not name (InvArgs). A decoder's Err means the reply is malformed; the
/// server's own failure is in the reply's status.
[[nodiscard]]
auto encode_isa_irq_reply(std::span<u8> bytes, u32 txid, IsaIrqReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_isa_irq_reply(wire::Message message, u32 txid)
    -> ustl::Result<IsaIrqReply, ours::Status>;

[[nodiscard]]
auto encode_ecam_base_reply(std::span<u8> bytes, u32 txid, EcamBaseReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_ecam_base_reply(wire::Message message, u32 txid)
    -> ustl::Result<EcamBaseReply, ours::Status>;
} // namespace ours_acpi

#endif // #ifndef OURS_ACPI_OURS_ACPI_HPP
