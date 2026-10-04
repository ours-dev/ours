/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LAUNCH_PROTOCOL_HPP
#define LAUNCH_PROTOCOL_HPP 1

#include <span>
#include <string_view>

#include <cstddef>

#include <ort/process_args.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>

namespace launch::protocol {
/// What a client sends the launcher service (system/service/launcher), and
/// what it answers. Ours' counterpart of fuchsia.process.Launcher, cut down
/// to one message per launch (.agents/tasks/launcher-service/DESIGN.md).
///
/// A connection is one channel. The client writes a request and, for a
/// Launch, waits for the reply before writing the next: one request at a
/// time per connection.

/// Which request a message is. Numbered from 1, so that a message of zeros
/// is no request at all.
enum class Ordinal : u32 {
    /// Start a program. Bytes: Header, the BootFS name (Header::name_size
    /// bytes, no NUL), zeros up to a multiple of 4, then a processargs
    /// message (<ours/process_args.hpp>) exactly as the program should get
    /// it, except that it has no VmaRoot: the launcher adds that. Handles:
    /// the job the process goes under, then the program's startup handles,
    /// in the order of that processargs' handle_info. Answered with a Reply,
    /// which on Ok carries the process handle.
    Launch = 1,

    /// Add a connection. Bytes: Header, name_size 0. Handles: one channel
    /// end, which the launcher serves from then on as it serves this one.
    /// Not answered. This is how a client that has a connection gives
    /// another program one of its own.
    Connect = 2,

    /// The answer to a Launch, sent by the launcher only. Its own ordinal,
    /// top bit set, so that an answer is never a request. The launcher closes
    /// a connection that sends it anything but Launch or Connect; without
    /// that, a client that handed the launcher both ends of one channel would
    /// have it read its own answer, answer that, and so on for ever. The
    /// planned wire format draws the same line with a direction field.
    LaunchReply = 0x8000'0001,
};

struct Header {
    Ordinal ordinal;
    u32 name_size;
};
static_assert(sizeof(Header) == 8);

/// The answer to a Launch: LaunchReply and how it went. On Ok the message
/// carries one handle, the new process, with the rights the launcher got from
/// process_create.
struct Reply {
    Ordinal ordinal;
    ours::Status status;
};
static_assert(sizeof(Reply) == 8);

/// The longest BootFS name a Launch may carry. Not NAME_MAX, which <limits.h>
/// defines as a macro in a host build, as MAX_NAME_LEN in <ours/syscall.hpp>
/// notes.
inline constexpr u32 MAX_PROGRAM_NAME = 255;

/// The most strings of each kind -- args, environ, names -- a Launch may
/// carry. A limit of the protocol's, not a consequence of the message size: a
/// launcher keeps the strings of the request it is serving in fixed arrays.
inline constexpr u32 MAX_STRINGS = 64;

/// Where a program finds its connection to the launcher among its startup
/// handles, when whoever started it gave it one.
inline constexpr auto CONNECTION = ours::HandleType::User1;

/// Writes a Launch request into |out| and its length into |length|: |name|,
/// then a processargs message of |handle_info|, |args|, |environ| and
/// |names|, as encode_process_args() lays one out.
///
/// |handle_info| describes the startup handles the caller sends after the
/// job, so it has one entry per handle and must not name VmaRoot, which the
/// launcher adds. InvArgs for an empty name, one longer than MAX_PROGRAM_NAME, or a
/// VmaRoot in |handle_info|; OutOfRange when the whole does not fit in |out|
/// or in one channel message, names more than CHANNEL_MAX_MSG_HANDLES - 1
/// handles (the job takes one), or has more than MAX_STRINGS strings of a
/// kind; otherwise encode_process_args()'s answer.
[[nodiscard]]
auto encode_launch(std::span<u8> out, std::string_view name, std::span<u32 const> handle_info,
                   std::span<std::string_view const> args,
                   std::span<std::string_view const> environ,
                   std::span<std::string_view const> names, u32 *length) -> ours::Status;

/// A Launch request, checked, and read in place: the name and the
/// processargs point into the message, which must outlive this.
struct Launch {
    std::string_view name;

    /// The processargs part, as it arrived.
    std::span<u8 const> process_args;

    /// The same, parsed: it carried |num_handles| - 1 handle_info entries,
    /// one per handle after the job.
    ort::ProcessArgs parsed;
};

/// Checks |message|, which arrived with |num_handles| handles, as a Launch
/// request, and on Ok fills |out|. Every failure is InvArgs:
///
///   - the message holds at least a Header, whose ordinal is Launch;
///   - the name is 1 to MAX_PROGRAM_NAME bytes and lies inside the message, as does
///     its padding;
///   - there is a job: |num_handles| is at least 1;
///   - what follows the padding is a processargs message that
///     ort::ProcessArgs::parse() accepts for |num_handles| - 1 handles, none
///     of whose handle_info names VmaRoot, and with no more than MAX_STRINGS
///     strings of a kind.
[[nodiscard]]
auto decode_launch(std::span<u8 const> message, u32 num_handles, Launch *out) -> ours::Status;

/// The ordinal of |message|, or none for one shorter than a Header.
[[nodiscard]]
auto ordinal_of(std::span<u8 const> message) -> ustl::Option<Ordinal>;
} // namespace launch::protocol

#endif // #ifndef LAUNCH_PROTOCOL_HPP
