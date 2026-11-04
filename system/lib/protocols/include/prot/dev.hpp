/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROT_DEV_HPP
#define PROT_DEV_HPP 1

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// ours.dev, the device tree's publish protocol (the device-namespace round,
/// .agents/tasks/device-namespace/DESIGN.md): what a driver calls to put one
/// of its devices into the registry's tree, and what the registry answers.
///
/// The tree answers a client's enumeration and opens by forwarding into the
/// published directories; the protocol carries only the publish itself.
///
///   Registry.Publish   two-way  u32 class_size@20, u32 name_size@24,
///                               u32 protocol_size@28, u32 directory@32
///                               (handle), char class[]@36, char name[]@36+class_size,
///                               char protocol[]@36+class_size+name_size
///
/// and its Ok reply, the status at 16 as in every reply:
///
///   Registry.Publish   u32 name_size@20, char name[]@24 -- the granted name
///
/// The registry owns the leaf's name: the driver proposes one, and the reply
/// says what was granted -- the proposal when it is free, otherwise the
/// class's lowest free three-digit number. A failure is the bare status form
/// and carries no name.
///
/// The class is the tree section the leaf sits in ("block"), the name the
/// driver's own numbering of its devices ("000", "001" -- publish order),
/// the protocol the wire name a client's open of the leaf is forwarded as
/// ("ours.block.Block"), and the directory handle the server end of the
/// driver's outgoing (or a subdir of it) that the forward goes into. Every decoder refuses
/// what is malformed with InvArgs; a class or name carrying '/' or a NUL is
/// refused, so a publisher cannot name a path.
namespace prot::dev {

/// The name a driver finds the registry under in its /svc, and the path a
/// driver's fsio::connect names it by.
inline constexpr auto REGISTRY_NAME = std::string_view{"ours.dev.Registry"};
inline constexpr auto REGISTRY_PATH = std::string_view{"/svc/ours.dev.Registry"};

/// The longest class name: one word, no separators.
inline constexpr auto MAX_CLASS_NAME = usize{64};

/// The longest node name a driver gives its device ("000").
inline constexpr auto MAX_NODE_NAME = usize{64};

/// The longest wire protocol name a leaf forwards its opens as
/// ("ours.block.Block").
inline constexpr auto MAX_PROTOCOL_NAME = usize{128};

/// The method: two-way, so the publisher learns the mount happened.
inline constexpr auto PUBLISH = wire::method("ours.dev/Registry.Publish", wire::MethodKind::TwoWay);

/// The publish. |kind| is the protocol family the leaf speaks ("block"),
/// |name| the driver's own numbering of the device, and |directory| the
/// server end opens of the leaf forward into.
struct PublishRequest {
    u32 txid;
    std::string_view kind;
    std::string_view name;
    std::string_view protocol;
    ours::Uandle directory;
};

/// The reply. |name| views the reply message's own bytes and says what was
/// granted: the proposal, or the class's lowest free number when it was
/// taken. A failure is written {.status = ...} and carries no name.
struct PublishReply {
    ours::Status status;
    std::string_view name;
};

/// Encoded by the driver: the strings behind the sizes, the directory as
/// handle 0. Refuses any of the three strings past its bound or carrying
/// '/' or a NUL, Uandle::Invalid, and a buffer too short.
[[nodiscard]]
auto encode_publish(std::span<u8> bytes, std::span<ours::Uandle> handles,
                    PublishRequest const &request) -> ustl::Result<wire::Message, ours::Status>;

/// Decoded by the registry. Refuses what is malformed with InvArgs; the
/// class and name view the message's own bytes.
[[nodiscard]]
auto decode_publish(wire::Message message) -> ustl::Result<PublishRequest, ours::Status>;

/// The registry's reply: Ok carries the granted name behind its size; a
/// failure is the bare status form.
[[nodiscard]]
auto encode_publish_reply(std::span<u8> bytes, u32 txid, PublishReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_publish_reply(wire::Message message, u32 txid)
    -> ustl::Result<PublishReply, ours::Status>;
} // namespace prot::dev

#endif // #ifndef PROT_DEV_HPP
