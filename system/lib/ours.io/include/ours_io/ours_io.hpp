/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IO_OURS_IO_HPP
#define OURS_IO_OURS_IO_HPP 1

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/util/enum_bits.hpp>
#include <wire/wire.hpp>

/// ours.io, the protocol files and directories are served over, written by
/// hand on <wire/wire.hpp> (component-framework design S6, D20, "The protocol
/// subset"): six methods and the epitaph, each kept because a step of the
/// plan uses it.
///
/// Each method's ordinal is computed from its selector, such as
/// "ours.io/File.Read", as this header is compiled (wire::method); the
/// bodies are flat, at fixed offsets. Offsets count from the start of the
/// message, so every body starts at 16:
///
///   Directory.Open          one-way  u64 flags@16, u32 object@24 (handle),
///                                    u32 path_size@28, char path[]@32
///   Node.Close              two-way  (nothing)
///   Cloneable.Clone         one-way  u32 request@16 (handle), u32 0@20
///   File.Read               two-way  u64 count@16
///   File.GetBackingMemory   two-way  u32 flags@16, u32 0@20
///   Directory.ReadDirents   two-way  u64 max_bytes@16
///
/// and the Ok replies, the status at 16 as in every reply (<wire/wire.hpp>):
///
///   Node.Close              the status form
///   File.Read               u32 size@20, u8 data[]@24
///   File.GetBackingMemory   u32 vmo@20 (handle)
///   Directory.ReadDirents   u32 size@20, u8 dirents[]@24
///
/// A message is exactly as long as its fields: a trailing array ends the
/// message. Any failure is the 24-byte status form, and a failed Open is an
/// epitaph on its |object|.
///
/// Every decoder refuses what is malformed with InvArgs, and the server or
/// client that called it closes the connection without an epitaph: a peer
/// that sends what does not decode is not speaking the protocol, so there is
/// no answer it could act on. What is well formed but wrong (an invalid path,
/// a count above the transfer limit, a right the connection lacks) is the
/// server's to answer.
namespace ours_io {
/// The longest name of one directory entry: what a dirent's u8 name_size can
/// count.
inline constexpr auto MAX_NAME_LENGTH = usize{255};

/// The longest path Open carries, which leaves room in a 4 KiB channel
/// message for Open's other fields. Raising it later, with the message size,
/// breaks no client: every path that fits now still fits.
inline constexpr auto MAX_PATH_LENGTH = usize{1024};

/// The most bytes one Read or ReadDirents reply carries: what is left of a
/// channel message after the header, the status and the size. MAX_BUF is the
/// name ReadDirents' bound goes by.
inline constexpr auto MAX_TRANSFER_SIZE =
    wire::MAX_MESSAGE_BYTES - (wire::HEADER_SIZE + sizeof(u32) + sizeof(u32));
inline constexpr auto MAX_BUF = MAX_TRANSFER_SIZE;

/// How Open is to be served: the rights asked for, the protocols the caller
/// accepts, and options. Every bit the protocol defines is named, those no
/// server here serves too, so that a server can refuse them by name.
///
/// Flags is flexible: a bit nobody defines decodes without complaint and is
/// the server's to judge, so a client that asks for more than a server knows
/// gets an answer from the server rather than a dropped connection.
enum class Flags : u64 {
    None = 0,
    PermConnect = 0x0001,
    PermReadBytes = 0x0002,
    PermWriteBytes = 0x0004,
    PermExecute = 0x0008,
    PermGetAttributes = 0x0010,
    PermUpdateAttributes = 0x0020,
    PermEnumerate = 0x0040,
    PermTraverse = 0x0080,
    PermModifyDirectory = 0x0100,
    PermInheritWrite = 0x2000,
    PermInheritExecute = 0x4000,
    ProtocolService = 0x0001'0000'0000,
    ProtocolNode = 0x0040'0000,
    ProtocolDirectory = 0x0008'0000,
    ProtocolFile = 0x0002'0000'0000,
    ProtocolSymlink = 0x0004'0000'0000,
    FlagSendRepresentation = 0x0100'0000'0000,
    FlagMaybeCreate = 0x0001'0000,
    FlagMustCreate = 0x0002'0000,
    FlagCreateAsUnnamedTemporary = 0x0008'0000'0000,
    FileAppend = 0x0010'0000,
    FileTruncate = 0x0004'0000,
};
USTL_ENABLE_ENUM_BITMASK(Flags);

/// The permission bits of Flags.
inline constexpr auto MASK_KNOWN_PERMISSIONS =
    Flags::PermReadBytes | Flags::PermWriteBytes | Flags::PermExecute |
    Flags::PermUpdateAttributes | Flags::PermEnumerate | Flags::PermModifyDirectory |
    Flags::PermConnect | Flags::PermGetAttributes | Flags::PermTraverse | Flags::PermInheritWrite |
    Flags::PermInheritExecute;

/// The protocol bits of Flags.
inline constexpr auto MASK_KNOWN_PROTOCOLS = Flags::ProtocolDirectory | Flags::ProtocolFile |
                                             Flags::ProtocolNode | Flags::ProtocolSymlink |
                                             Flags::ProtocolService;

/// What Open asks for to read, write or run what it opens.
inline constexpr auto PERM_READABLE = Flags::PermConnect | Flags::PermEnumerate |
                                      Flags::PermTraverse | Flags::PermReadBytes |
                                      Flags::PermGetAttributes;
inline constexpr auto PERM_WRITABLE = Flags::PermConnect | Flags::PermEnumerate |
                                      Flags::PermTraverse | Flags::PermWriteBytes |
                                      Flags::PermModifyDirectory | Flags::PermUpdateAttributes;
inline constexpr auto PERM_EXECUTABLE =
    Flags::PermConnect | Flags::PermEnumerate | Flags::PermTraverse | Flags::PermExecute;

/// What a connection may do, and what a node can. The bits are those of
/// Flags' PERM_* bits, which is how Open's request becomes a connection's
/// rights.
enum class Operations : u64 {
    None = 0,
    Connect = 0x01,
    ReadBytes = 0x02,
    WriteBytes = 0x04,
    Execute = 0x08,
    GetAttributes = 0x10,
    UpdateAttributes = 0x20,
    Enumerate = 0x40,
    Traverse = 0x80,
    ModifyDirectory = 0x100,
};
USTL_ENABLE_ENUM_BITMASK(Operations);

/// The rights words a manifest uses: r*, rw*, rx*, w* and x*, one constant
/// each. The built-in `boot` directory is rx*.
inline constexpr auto R_STAR_DIR = Operations::Connect | Operations::Enumerate |
                                   Operations::Traverse | Operations::ReadBytes |
                                   Operations::GetAttributes;
inline constexpr auto RW_STAR_DIR = Operations::Connect | Operations::Enumerate |
                                    Operations::Traverse | Operations::ReadBytes |
                                    Operations::WriteBytes | Operations::ModifyDirectory |
                                    Operations::GetAttributes | Operations::UpdateAttributes;
inline constexpr auto RX_STAR_DIR = Operations::Connect | Operations::Enumerate |
                                    Operations::Traverse | Operations::ReadBytes |
                                    Operations::GetAttributes | Operations::Execute;
inline constexpr auto W_STAR_DIR = Operations::Connect | Operations::Enumerate |
                                   Operations::Traverse | Operations::WriteBytes |
                                   Operations::ModifyDirectory | Operations::UpdateAttributes;
inline constexpr auto X_STAR_DIR =
    Operations::Connect | Operations::Enumerate | Operations::Traverse | Operations::Execute;

/// What GetBackingMemory is asked for.
///
/// VmoFlags is strict: a bit it does not define makes the request malformed,
/// and the decoder refuses it. Contrast Flags.
enum class VmoFlags : u32 {
    None = 0,
    Read = 0x0000'0001,
    Write = 0x0000'0002,
    Execute = 0x0000'0004,
    PrivateClone = 0x0001'0000,
    SharedBuffer = 0x0002'0000,
};
USTL_ENABLE_ENUM_BITMASK(VmoFlags);

/// Every bit VmoFlags defines.
inline constexpr auto VMO_FLAGS_KNOWN = VmoFlags::Read | VmoFlags::Write | VmoFlags::Execute |
                                        VmoFlags::PrivateClone | VmoFlags::SharedBuffer;

/// The type of a directory entry, POSIX's d_type values. DirentType is
/// flexible: any value reads back.
enum class DirentType : u8 {
    Unknown = 0,
    Directory = 4,
    BlockDevice = 6,
    File = 8,
    Symlink = 10,
    Service = 16,
};

/// The inode number of an entry that has none.
inline constexpr auto INO_UNKNOWN = u64{0xFFFF'FFFF'FFFF'FFFF};

// The methods, each declared by its selector and kind alone: wire::method()
// computes the ordinal from the selector, so there is no number to type.

/// Opens |path|, relative to the directory the request is written to, on
/// the channel |object|.
inline constexpr auto OPEN = wire::method("ours.io/Directory.Open", wire::MethodKind::OneWay);

/// Closes the connection after answering.
inline constexpr auto CLOSE = wire::method("ours.io/Node.Close", wire::MethodKind::TwoWay);

/// Serves another connection to the same node, with the same rights, on
/// |request|.
inline constexpr auto CLONE = wire::method("ours.io/Cloneable.Clone", wire::MethodKind::OneWay);

/// Reads up to |count| bytes at the seek offset and moves it.
inline constexpr auto READ = wire::method("ours.io/File.Read", wire::MethodKind::TwoWay);

/// A VMO of the file's contents, with the rights |flags| ask for.
inline constexpr auto GET_BACKING_MEMORY =
    wire::method("ours.io/File.GetBackingMemory", wire::MethodKind::TwoWay);

/// As many whole entries as fit in |max_bytes|, from the directory's cursor.
inline constexpr auto READ_DIRENTS =
    wire::method("ours.io/Directory.ReadDirents", wire::MethodKind::TwoWay);

// A connection answers all of these on one channel, whichever node it is on, so
// no two may share an ordinal. (Ordinals have to be distinct only within a
// protocol; a header lists the methods one channel can carry.)
static_assert(wire::all_distinct(OPEN, CLOSE, CLONE, READ, GET_BACKING_MEMORY, READ_DIRENTS),
              "two ours.io methods have the same ordinal");

/// Directory.Open. |path| points into the message it was decoded from.
struct OpenRequest {
    Flags flags;
    std::string_view path;
    ours::Uandle object;
};

struct CloseRequest {
    u32 txid;
};

struct CloneRequest {
    ours::Uandle request;
};

struct ReadRequest {
    u32 txid;
    u64 count;
};

struct GetBackingMemoryRequest {
    u32 txid;
    VmoFlags flags;
};

struct ReadDirentsRequest {
    u32 txid;
    u64 max_bytes;
};

/// The replies. |status| is the server's answer; when it is not Ok, the rest
/// is empty, which is what the defaults say, so that a failure is written
/// {.status = ...}.
struct CloseReply {
    ours::Status status;
};

struct ReadReply {
    ours::Status status;
    std::span<u8 const> data = {};
};

struct GetBackingMemoryReply {
    ours::Status status;
    ours::Uandle vmo = ours::Uandle::Invalid;
};

struct ReadDirentsReply {
    ours::Status status;
    std::span<u8 const> dirents = {};
};

// Requests, encoded by a client into |bytes| (and |handles|, for a request
// that carries one) and decoded by a server. An encoder returns the message
// to write: the front of |bytes| and of |handles|. It refuses what the
// protocol cannot carry with InvArgs (a path longer than MAX_PATH_LENGTH, a
// VmoFlags bit the protocol does not define, Uandle::Invalid for a handle, a
// txid outside 1 to MAX_TXID) and a buffer too short with BufferTooSmall.
// What the protocol carries but the server will refuse, such as an empty
// path, it encodes.

[[nodiscard]]
auto encode_open(std::span<u8> bytes, std::span<ours::Uandle> handles, OpenRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_open(wire::Message message) -> ustl::Result<OpenRequest, ours::Status>;

[[nodiscard]]
auto encode_close(std::span<u8> bytes, CloseRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_close(wire::Message message) -> ustl::Result<CloseRequest, ours::Status>;

[[nodiscard]]
auto encode_clone(std::span<u8> bytes, std::span<ours::Uandle> handles, CloneRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_clone(wire::Message message) -> ustl::Result<CloneRequest, ours::Status>;

[[nodiscard]]
auto encode_read(std::span<u8> bytes, ReadRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_read(wire::Message message) -> ustl::Result<ReadRequest, ours::Status>;

[[nodiscard]]
auto encode_get_backing_memory(std::span<u8> bytes, GetBackingMemoryRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_get_backing_memory(wire::Message message)
    -> ustl::Result<GetBackingMemoryRequest, ours::Status>;

[[nodiscard]]
auto encode_read_dirents(std::span<u8> bytes, ReadDirentsRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_read_dirents(wire::Message message) -> ustl::Result<ReadDirentsRequest, ours::Status>;

// Replies to the request |txid|, encoded by the server and decoded by the
// client that sent it. A reply whose status is not Ok is the status form and
// must carry nothing else: the encoder refuses data, dirents or a VMO next to
// a failure (InvArgs). A decoder's Err means the reply is malformed; the
// server's own failure is in the reply's |status|.

[[nodiscard]]
auto encode_close_reply(std::span<u8> bytes, u32 txid, CloseReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_close_reply(wire::Message message, u32 txid) -> ustl::Result<CloseReply, ours::Status>;

/// The data may already lie where the reply puts it, at reply_payload(bytes):
/// a server can build it in place.
[[nodiscard]]
auto encode_read_reply(std::span<u8> bytes, u32 txid, ReadReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_read_reply(wire::Message message, u32 txid) -> ustl::Result<ReadReply, ours::Status>;

[[nodiscard]]
auto encode_get_backing_memory_reply(std::span<u8> bytes, std::span<ours::Uandle> handles, u32 txid,
                                     GetBackingMemoryReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_get_backing_memory_reply(wire::Message message, u32 txid)
    -> ustl::Result<GetBackingMemoryReply, ours::Status>;

/// As encode_read_reply(): the dirents may already lie at
/// reply_payload(bytes).
[[nodiscard]]
auto encode_read_dirents_reply(std::span<u8> bytes, u32 txid, ReadDirentsReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_read_dirents_reply(wire::Message message, u32 txid)
    -> ustl::Result<ReadDirentsReply, ours::Status>;

/// Where a Read or ReadDirents reply in |bytes| puts its data: the bytes from
/// 24 on, at most MAX_TRANSFER_SIZE of them. A server that writes the data
/// there first saves a copy. Empty when |bytes| is no longer than 24.
[[nodiscard]]
auto reply_payload(std::span<u8> bytes) -> std::span<u8>;

/// One directory entry as ReadDirents packs it: u64 ino, u8 name_size,
/// u8 type, then the name, unaligned and unterminated. |name| points into
/// the bytes it was read from.
struct Dirent {
    u64 ino;
    DirentType type;
    std::string_view name;
};

/// The bytes of a dirent before its name.
inline constexpr auto DIRENT_HEADER_SIZE = usize{10};

/// Packs dirents into a buffer, one after another, for a ReadDirents reply.
class DirentWriter final {
  public:
    explicit DirentWriter(std::span<u8> buffer) : m_buffer(buffer) {}

    /// Appends |dirent|. BufferTooSmall when it does not fit in what is left,
    /// which is left as it was: the caller stops there and sends that entry
    /// first in its next reply. InvArgs for an empty name or one longer than
    /// MAX_NAME_LENGTH. The name itself is the server's to choose: "." is a
    /// valid entry, the first a directory lists.
    [[nodiscard]]
    auto append(Dirent const &dirent) -> ours::Status;

    /// What has been appended.
    [[nodiscard]]
    auto written() const -> std::span<u8 const> {
        return m_buffer.first(m_size);
    }

  private:
    std::span<u8> m_buffer;
    usize m_size = 0;
};

/// Reads dirents back out of a ReadDirents reply.
class DirentReader final {
  public:
    explicit DirentReader(std::span<u8 const> dirents) : m_rest(dirents) {}

    /// The next entry, or none after the last. InvArgs when an entry is cut
    /// short or has an empty name; the reader then stays where it is.
    [[nodiscard]]
    auto next() -> ustl::Result<ustl::Option<Dirent>, ours::Status>;

  private:
    std::span<u8 const> m_rest;
};
} // namespace ours_io

#endif // #ifndef OURS_IO_OURS_IO_HPP
