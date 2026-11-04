/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROT_FSHOST_HPP
#define PROT_FSHOST_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <array>
#include <span>
#include <string_view>

#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// ours.fshost's admin protocol, the one verb that grows the macro
/// filesystem at run time: Mount hands the composing server a directory
/// capability and a name, and the name appears under the writable view --
/// opens below it walk into the mounted filesystem.
///
///   Admin.Mount       two-way  directory handle, u32 name size@16, u8 name@20
///
/// and its reply, the status at 16 as in every reply. Ok means the mount
/// point exists from the next Open onward; connections already bound are
/// untouched. AlreadyExists when the name is taken, InvArgs for an empty
/// or oversized name.
///
/// Unmount is deliberately absent for now: taking a mount point down
/// wants a rule for the connections walking it, which is its own design.
namespace prot::fshost {
/// The name the protocol is routed by, and the one fshost serves it under
/// in its outgoing directory's svc/.
inline constexpr auto ADMIN_NAME = std::string_view{"ours.fshost.Admin"};

/// The most bytes of a mount point's name, ours.io's own cap for a name:
/// a mount point is a name in a directory, and the two should not differ
/// in what they accept.
inline constexpr auto MAX_NAME_BYTES = usize{255};

/// Admin.Mount: wire::method hashes the selector at compile time.
inline constexpr auto MOUNT = wire::method("ours.fshost/Admin.Mount", wire::MethodKind::TwoWay);

struct MountRequest {
    u32 txid;
    /// The mounted filesystem's directory capability, the message's one
    /// handle.
    ours::Uandle handle = ours::Uandle::Invalid;
    std::string_view name;
};

/// InvArgs for an empty or oversized name.
[[nodiscard]]
auto encode_mount(std::span<u8> bytes, std::span<ours::Uandle> handles, MountRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

/// The directory handle rides the message's handles; the name views the
/// message's own bytes. InvArgs for a shape that cannot be a Mount.
[[nodiscard]]
auto decode_mount(wire::Message message) -> ustl::Result<MountRequest, ours::Status>;

/// The reply is the status form, whatever the status.
[[nodiscard]]
auto encode_mount_reply(std::span<u8> bytes, u32 txid, ours::Status status)
    -> ustl::Result<wire::Message, ours::Status>;
} // namespace prot::fshost

#endif // #ifndef PROT_FSHOST_HPP
