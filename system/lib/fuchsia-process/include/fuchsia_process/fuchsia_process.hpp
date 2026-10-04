/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FUCHSIA_PROCESS_FUCHSIA_PROCESS_HPP
#define FUCHSIA_PROCESS_FUCHSIA_PROCESS_HPP 1

#include <span>
#include <string_view>

#include <fuchsia_io/fuchsia_io.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The part of fuchsia.process.Launcher that ours speaks, written by hand on
/// <wire/wire.hpp> (component-framework design S6, D13, D20, "The protocol
/// subset"): the stateful Add* calls and Launch, which is what fdio_spawn
/// sends (sdk/lib/fdio/spawn.cc). CreateWithoutStarting, AddEnvirons and
/// SetOptions are cut (design non-goals).
///
/// The names and the ordinals are Fuchsia's (sdk/fidl/fuchsia.process/
/// launcher.fidl); the bodies are flat, at fixed offsets from the start of
/// the message:
///
///   Launcher.AddArgs     one-way  u32 count@16, u32 size@20, u8 args[size]@24
///   Launcher.AddHandles  one-way  u32 count@16, u32 0@20,
///                                 {u32 handle, u32 id}[count]@24
///   Launcher.AddNames    one-way  u32 count@16, u32 size@20,
///                                 u32 directory[count]@24 (handles),
///                                 u8 paths[size]@24+4*count
///   Launcher.Launch      two-way  u32 executable@16 (handle), u32 job@20
///                                 (handle), u32 name_size@24, char name[]@28
///
/// and Launch's Ok reply, the status at 16 as in every reply:
///
///   Launcher.Launch      u32 process@20 (handle)
///
/// A message is exactly as long as its fields. A failed Launch is the 24-byte
/// status form, as Fuchsia's reply carries no process unless the status is
/// ZX_OK.
///
/// Strings travel packed: |count| strings, each ended by a NUL, back to back,
/// |size| bytes in all. That is processargs' own layout for its string
/// sections (<ours/process_args.hpp>), so a launcher copies them into the
/// processargs it writes as they are. FIDL sends each string as a
/// vector<uint8>, which may hold a NUL, and Fuchsia's Launcher answers
/// INVALID_ARGS at Launch for one that does (src/sys/lib/elf_runner/src/
/// process_launcher.rs, "Argument contained null byte"); here a NUL ends a
/// string, so an encoder refuses a string that holds one, and no server ever
/// sees one.
///
/// Launcher is a closed protocol, so a message whose ordinal is none of these
/// is no Launcher request, and the server closes the connection. Every
/// decoder refuses what is malformed with InvArgs, and the server closes the
/// connection without an epitaph, as Fuchsia's Launcher drops a channel whose
/// message fails to decode (process_launcher.rs, ProcessLauncher::serve).
/// What is well formed but cannot be launched (a reserved handle type, more
/// handles or bytes than a processargs can carry, a NUL in the process name)
/// is the server's to answer at Launch.
namespace fuchsia_process {
/// The name a component finds the Launcher under in its /svc
/// (@discoverable).
inline constexpr auto LAUNCHER_NAME = std::string_view{"fuchsia.process.Launcher"};

/// The longest process name Launch carries: LaunchInfo.name is
/// string:zx.MAX_NAME_LEN. process_create keeps the first 31 bytes of it.
inline constexpr auto MAX_NAME_LEN = usize{ours::MAX_NAME_LEN};

/// The longest namespace path AddNames carries. NameInfo.path is a
/// fuchsia.io.Path (launcher.fidl uses fuchsia.io), so the bound is
/// fuchsia.io's, as ours sizes it.
inline constexpr auto MAX_PATH_LENGTH = fuchsia_io::MAX_PATH_LENGTH;

// The methods, each declared beside the selector its ordinal is hashed from.
// wire/test/ordinals_test.py recomputes every ordinal from its selector.

/// Appends to the command line of the next process (Launcher.AddArgs).
inline constexpr auto ADD_ARGS = wire::Method{
    .selector = "fuchsia.process/Launcher.AddArgs",
    .ordinal = 0x3be4'45d3'e4fd'6512,
    .kind = wire::MethodKind::OneWay,
};

/// Appends to the startup handles of the next process
/// (Launcher.AddHandles).
inline constexpr auto ADD_HANDLES = wire::Method{
    .selector = "fuchsia.process/Launcher.AddHandles",
    .ordinal = 0x5102'5267'a537'a615,
    .kind = wire::MethodKind::OneWay,
};

/// Appends to the namespace of the next process (Launcher.AddNames).
inline constexpr auto ADD_NAMES = wire::Method{
    .selector = "fuchsia.process/Launcher.AddNames",
    .ordinal = 0x2579'ee2c'7be2'8662,
    .kind = wire::MethodKind::OneWay,
};

/// Creates and starts the process, with what the Add* calls gave; the
/// connection is then as new (Launcher.Launch).
inline constexpr auto LAUNCH = wire::Method{
    .selector = "fuchsia.process/Launcher.Launch",
    .ordinal = 0x1133'5a99'28af'bfa4,
    .kind = wire::MethodKind::TwoWay,
};

/// One startup handle and what it is for (fuchsia.process.HandleInfo): |id|
/// is a handle_info of <ours/process_args.hpp>, the type in bits 0-7 and its
/// argument in bits 16-31.
struct HandleInfo {
    ours::Uandle handle;
    u32 id;
};

/// One namespace entry (fuchsia.process.NameInfo): a client end of a
/// fuchsia.io directory and the absolute path it is to be bound at.
struct NameInfo {
    std::string_view path;
    ours::Uandle directory;
};

/// What to start (fuchsia.process.LaunchInfo).
struct LaunchInfo {
    /// The program, as a VMO.
    ours::Uandle executable;
    /// The job the process goes under.
    ours::Uandle job;
    /// The process's name, at most MAX_NAME_LEN bytes.
    std::string_view name;
};

/// Strings as AddArgs and AddNames carry them: |count| strings, each ended by
/// a NUL, back to back. Decoded, so |bytes| holds exactly |count| NULs and
/// ends in one.
struct PackedStrings {
    u32 count;
    std::span<u8 const> bytes;
};

/// Reads packed strings back one by one.
class StringReader final {
  public:
    explicit StringReader(PackedStrings const strings) : m_rest(strings.bytes) {}

    /// The next string, without its NUL, or none after the last. Bytes
    /// after the last NUL, which a decoder never leaves, end the reading.
    [[nodiscard]]
    auto next() -> ustl::Option<std::string_view>;

  private:
    std::span<u8 const> m_rest;
};

/// AddArgs, decoded. The strings point into the message.
struct AddArgsRequest {
    PackedStrings args;
};

/// AddHandles, decoded and read in place: entry |index| is at(index).
struct AddHandlesRequest {
    /// The handles, in the order of their entries.
    std::span<ours::Uandle const> handles;
    /// The entries as they lie in the message, {u32 handle, u32 id} each.
    std::span<u8 const> entries;

    /// Entry |index|, which must be below handles.size().
    [[nodiscard]]
    auto at(usize index) const -> HandleInfo;
};

/// AddNames, decoded: path |i| of |paths| goes with directories[i]. The
/// paths point into the message.
struct AddNamesRequest {
    PackedStrings paths;
    std::span<ours::Uandle const> directories;
};

/// Launch. |info.name| points into the message it was decoded from.
struct LaunchRequest {
    u32 txid;
    LaunchInfo info;
};

/// Launch's reply. |status| is the launcher's answer; |process| is there
/// when, and only when, it is Ok, so a failure is written {.status = ...}.
struct LaunchReply {
    ours::Status status;
    ours::Uandle process = ours::Uandle::Invalid;
};

// Requests, encoded by a client into |bytes| (and |handles|, for a request
// that carries any) and decoded by a server. An encoder returns the message
// to write: the front of |bytes| and of |handles|. It refuses what the
// protocol cannot carry with InvArgs (a string that holds a NUL, a path
// longer than MAX_PATH_LENGTH, a name longer than MAX_NAME_LEN,
// Uandle::Invalid for a handle, more handles or bytes than one message
// carries, a txid outside 1 to MAX_TXID) and a buffer too short with
// BufferTooSmall. What the protocol carries but a launcher will refuse, such
// as a reserved handle type, it encodes.

[[nodiscard]]
auto encode_add_args(std::span<u8> bytes, std::span<std::string_view const> args)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_add_args(wire::Message message) -> ustl::Result<AddArgsRequest, ours::Status>;

[[nodiscard]]
auto encode_add_handles(std::span<u8> bytes, std::span<ours::Uandle> handles,
                        std::span<HandleInfo const> infos)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_add_handles(wire::Message message) -> ustl::Result<AddHandlesRequest, ours::Status>;

[[nodiscard]]
auto encode_add_names(std::span<u8> bytes, std::span<ours::Uandle> handles,
                      std::span<NameInfo const> names) -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_add_names(wire::Message message) -> ustl::Result<AddNamesRequest, ours::Status>;

[[nodiscard]]
auto encode_launch(std::span<u8> bytes, std::span<ours::Uandle> handles,
                   LaunchRequest const &request) -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_launch(wire::Message message) -> ustl::Result<LaunchRequest, ours::Status>;

// Launch's reply to the request |txid|, encoded by the launcher and decoded
// by the client that sent it. A reply whose status is not Ok is the status
// form: the encoder refuses a process next to a failure (InvArgs). A
// decoder's Err means the reply is malformed; the launcher's own failure is
// in the reply's |status|.

[[nodiscard]]
auto encode_launch_reply(std::span<u8> bytes, std::span<ours::Uandle> handles, u32 txid,
                         LaunchReply const &reply) -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_launch_reply(wire::Message message, u32 txid)
    -> ustl::Result<LaunchReply, ours::Status>;
} // namespace fuchsia_process

#endif // #ifndef FUCHSIA_PROCESS_FUCHSIA_PROCESS_HPP
