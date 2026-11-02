/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// ioservice::Server (<ioservice/server.hpp>), driven the way the root will drive it:
// one encoded ours.io message at a time, with the Outcome read back as a
// program would act on it. No channel exists; handles are numbers, and the
// one system call the server makes is a stand-in that records what it was
// asked.
#include <ioservice/server.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <ours_io/ours_io.hpp>
#include <wire/wire.hpp>

namespace {
using ioservice::Action;
using ioservice::ConnectionId;
using ioservice::NodeId;
using ioservice::Outcome;
using ours::Rights;
using ours::Status;
using ours::Uandle;
using ours_io::Flags;
using ours_io::Operations;
using ours_io::VmoFlags;

constexpr auto FILL = u8{0xCC};
constexpr auto TXID = u32{5};
constexpr auto CHANNEL = Uandle{0x1001};
constexpr auto OBJECT = Uandle{0x1101};
constexpr auto OTHER_OBJECT = Uandle{0x1102};
constexpr auto VMO_X = Uandle{0x2001};
constexpr auto VMO_README = Uandle{0x2002};
constexpr auto DUPLICATE = Uandle{0x3001};
constexpr auto REMOTE = Uandle{0x4001};
constexpr auto ECHO = u64{7};

constexpr auto README = std::string_view{"hello, world"};
constexpr auto X_CONTENTS = std::string_view{"\x7f"
                                             "ELF"};

/// What the stand-in for handle_duplicate was asked, and what it answers.
struct Duplicates {
    u32 calls = 0;
    Uandle handle = Uandle::Invalid;
    Rights rights = Rights::None;
    Status answer = Status::Ok;
};

auto duplicates() -> Duplicates & {
    static auto LOG = Duplicates{};
    return LOG;
}

auto fake_duplicate(Uandle const handle, Rights const rights, Uandle *const out) -> Status {
    auto &log = duplicates();
    ++log.calls;
    log.handle = handle;
    log.rights = rights;
    if (log.answer != Status::Ok) {
        return log.answer;
    }
    *out = DUPLICATE;
    return Status::Ok;
}

auto bytes_of(std::string_view const text) -> std::span<u8 const> {
    return {reinterpret_cast<u8 const *>(text.data()), text.size()};
}

auto text_of(std::span<u8 const> const bytes) -> std::string {
    return {reinterpret_cast<char const *>(bytes.data()), bytes.size()};
}

/// A ReadDirents reply, read back.
struct Listing {
    Status status;
    std::vector<std::string> names;
    std::vector<ours_io::DirentType> types;
};

/// The tree every test serves:
///
///   root/                 a directory
///     bin/
///       x                 an executable file
///     config/
///       readme            a plain file, "hello, world"
///   svc/                  a second root, as one component's /svc
///     echo                a service
class IoserviceServer: public testing::Test {
  protected:
    IoserviceServer() {
        duplicates() = Duplicates{};
        m_root = m_tree.add_root().unwrap();
        m_bin = m_tree.add_directory(m_root, "bin").unwrap();
        m_x = m_tree
                  .add_file(m_bin, "x",
                            {.vmo = VMO_X, .bytes = bytes_of(X_CONTENTS), .executable = true})
                  .unwrap();
        m_config = m_tree.add_directory(m_root, "config").unwrap();
        m_readme =
            m_tree
                .add_file(m_config, "readme",
                          {.vmo = VMO_README, .bytes = bytes_of(README), .executable = false})
                .unwrap();
        m_svc = m_tree.add_root().unwrap();
        m_echo = m_tree.add_service(m_svc, "echo", ECHO).unwrap();
        // A second tree to mount through, kept apart from the first so that
        // no listing of / changes with these tests.
        m_mount = m_tree.add_root().unwrap();
        m_mnt = m_tree.add_directory(m_mount, "mnt").unwrap();
        m_boot = m_tree.add_remote(m_mnt, "boot", REMOTE).unwrap();
    }

    auto bind(NodeId const node, Operations const rights) -> ConnectionId {
        auto const bound = m_server.bind(CHANNEL, node, rights);
        EXPECT_TRUE(bound.is_ok());
        if (bound.is_err()) {
            return {};
        }
        return bound.unwrap();
    }

    /// Hands |message| to the server, as a Loop callback would.
    auto dispatch(ConnectionId const connection, ustl::Result<wire::Message, Status> const &message)
        -> Outcome {
        EXPECT_TRUE(message.is_ok());
        if (message.is_err()) {
            return Outcome{.action = Action::Drop};
        }
        return m_server.dispatch(connection, message.unwrap(), m_out);
    }

    auto open(ConnectionId const connection, std::string_view const path, Flags const flags,
              Uandle const object = OBJECT) -> Outcome {
        return dispatch(connection,
                        ours_io::encode_open(m_bytes, m_handles,
                                             {.flags = flags, .path = path, .object = object}));
    }

    /// An Open that must succeed with a new connection.
    auto opened(ConnectionId const connection, std::string_view const path, Flags const flags)
        -> ConnectionId {
        auto const outcome = open(connection, path, flags);
        EXPECT_EQ(outcome.action, Action::Bind) << path;
        EXPECT_EQ(outcome.handle, OBJECT);
        EXPECT_EQ(held(m_server.channel(outcome.connection)), OBJECT);
        return outcome.connection;
    }

    /// The status an Open is refused with, read back from its epitaph.
    auto refused(ConnectionId const connection, std::string_view const path, Flags const flags)
        -> Status {
        auto const outcome = open(connection, path, flags);
        EXPECT_EQ(outcome.action, Action::Refuse) << path;
        EXPECT_EQ(outcome.handle, OBJECT);
        return epitaph(outcome);
    }

    static auto epitaph(Outcome const &outcome) -> Status {
        auto const status = wire::decode_epitaph(outcome.message);
        EXPECT_TRUE(status.is_ok());
        if (status.is_err()) {
            return Status::InternalError;
        }
        return status.unwrap();
    }

    auto read(ConnectionId const connection, u64 const count) -> ours_io::ReadReply {
        auto const outcome =
            dispatch(connection, ours_io::encode_read(m_bytes, {.txid = TXID, .count = count}));
        EXPECT_EQ(outcome.action, Action::Reply);
        auto const reply = ours_io::decode_read_reply(outcome.message, TXID);
        EXPECT_TRUE(reply.is_ok());
        if (reply.is_err()) {
            return {.status = Status::InternalError};
        }
        return reply.unwrap();
    }

    auto read_dirents(ConnectionId const connection, u64 const max_bytes) -> Listing {
        auto const outcome =
            dispatch(connection,
                     ours_io::encode_read_dirents(m_bytes, {.txid = TXID, .max_bytes = max_bytes}));
        EXPECT_EQ(outcome.action, Action::Reply);
        auto const reply = ours_io::decode_read_dirents_reply(outcome.message, TXID);
        EXPECT_TRUE(reply.is_ok());
        if (reply.is_err()) {
            return {.status = Status::InternalError, .names = {}, .types = {}};
        }
        auto listing = Listing{.status = reply->status, .names = {}, .types = {}};
        auto reader = ours_io::DirentReader(reply->dirents);
        for (auto next = reader.next(); next.is_ok() && next->is_some(); next = reader.next()) {
            auto const &dirent = next.unwrap().value();
            EXPECT_EQ(dirent.ino, ours_io::INO_UNKNOWN);
            listing.names.emplace_back(dirent.name);
            listing.types.push_back(dirent.type);
        }
        return listing;
    }

    auto get_backing_memory(ConnectionId const connection, VmoFlags const flags)
        -> ours_io::GetBackingMemoryReply {
        auto const outcome =
            dispatch(connection,
                     ours_io::encode_get_backing_memory(m_bytes, {.txid = TXID, .flags = flags}));
        EXPECT_EQ(outcome.action, Action::Reply);
        auto const reply = ours_io::decode_get_backing_memory_reply(outcome.message, TXID);
        EXPECT_TRUE(reply.is_ok());
        if (reply.is_err()) {
            return {.status = Status::InternalError};
        }
        return reply.unwrap();
    }

    /// What a ReadDirents on |connection| comes to, for an id that may be
    /// stale.
    auto read_dirents_outcome(ConnectionId const connection) -> Action {
        return dispatch(connection, ours_io::encode_read_dirents(
                                        m_bytes, {.txid = TXID, .max_bytes = ours_io::MAX_BUF}))
            .action;
    }

    auto clone(ConnectionId const connection, Uandle const request) -> Outcome {
        return dispatch(connection,
                        ours_io::encode_clone(m_bytes, m_handles, {.request = request}));
    }

    template <typename T>
    static auto held(ustl::Option<T> const &option) -> T {
        EXPECT_TRUE(option.is_some());
        if (option.is_none()) {
            return T{};
        }
        return *option;
    }

    std::array<ioservice::Node, 16> m_nodes{};
    ioservice::Tree m_tree{m_nodes};
    std::array<ioservice::Connection, 4> m_connections{};
    ioservice::Server m_server{m_tree, m_connections, {.handle_duplicate = fake_duplicate}};
    ioservice::ReplyBuffer m_out{};

    /// Where requests are encoded, starting dirty so that a byte an encoder
    /// forgot would show.
    std::vector<u8> m_bytes = std::vector<u8>(wire::MAX_MESSAGE_BYTES, FILL);
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> m_handles{};

    NodeId m_root{};
    NodeId m_bin{};
    NodeId m_x{};
    NodeId m_config{};
    NodeId m_readme{};
    NodeId m_svc{};
    NodeId m_echo{};
    NodeId m_mount{};
    NodeId m_mnt{};
    NodeId m_boot{};
};

// --- Open: the checks, in their fixed order ----------------------------------

TEST_F(IoserviceServer, OpenJudgesThePathFirst) {
    // A connection with no rights at all: a bad path is still BadPath, and a
    // good one is refused for want of TRAVERSE.
    auto const root = bind(m_root, Operations::None);
    for (auto const path: {"/bin", "bin/", "", "..", "bin/../config", "bin//x"}) {
        EXPECT_EQ(refused(root, path, Flags::None), Status::BadPath) << path;
    }
    EXPECT_EQ(refused(root, "bin", Flags::None), Status::AccessDenied);
}

TEST_F(IoserviceServer, OpenNeedsTraverseExceptForDot) {
    auto const root = bind(m_root, Operations::Enumerate);
    auto const dot = opened(root, ".", Flags::PermEnumerate);
    EXPECT_EQ(held(m_server.rights(dot)), Operations::Enumerate);
    EXPECT_EQ(held(m_server.node(dot)), m_root);
    // AccessDenied whether or not the name exists: without TRAVERSE, nothing
    // below the directory can be learnt.
    EXPECT_EQ(refused(root, "bin", Flags::None), Status::AccessDenied);
    EXPECT_EQ(refused(root, "nope", Flags::None), Status::AccessDenied);
}

TEST_F(IoserviceServer, OpenChecksRightsBeforeTheTree) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "nope", Flags::PermWriteBytes), Status::AccessDenied);
    EXPECT_EQ(refused(root, "bin", Flags::PermModifyDirectory), Status::AccessDenied);
    EXPECT_EQ(refused(root, "nope", ours_io::PERM_READABLE), Status::NotFound);
}

TEST_F(IoserviceServer, OpenChecksConnectForProtocolService) {
    auto const no_connect = bind(m_svc, Operations::Enumerate | Operations::Traverse);
    EXPECT_EQ(refused(no_connect, "echo", Flags::ProtocolService), Status::AccessDenied);
    // Before the lookup: a missing name answers the same.
    EXPECT_EQ(refused(no_connect, "nope", Flags::ProtocolService), Status::AccessDenied);

    auto const svc = bind(m_svc, ours_io::R_STAR_DIR);
    EXPECT_EQ(refused(svc, "nope", Flags::ProtocolService), Status::NotFound);
}

TEST_F(IoserviceServer, OpenRefusesToCreate) {
    // MODIFY_DIRECTORY first. With it the answer is still NotSupported,
    // before the tree is looked at.
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "new", Flags::FlagMaybeCreate | Flags::ProtocolFile),
              Status::AccessDenied);

    auto const writable = bind(m_root, ours_io::RX_STAR_DIR | Operations::ModifyDirectory);
    EXPECT_EQ(refused(writable, "new", Flags::FlagMaybeCreate | Flags::ProtocolFile),
              Status::NotSupported);
    EXPECT_EQ(refused(writable, "bin", Flags::FlagMustCreate | Flags::ProtocolDirectory),
              Status::NotSupported);
    EXPECT_EQ(refused(writable, "config/readme", Flags::FlagCreateAsUnnamedTemporary),
              Status::NotSupported);
}

TEST_F(IoserviceServer, OpenRefusesWhatTheDesignCut) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "bin", Flags::ProtocolNode), Status::NotSupported);
    EXPECT_EQ(refused(root, "bin", Flags::FlagSendRepresentation), Status::NotSupported);
    EXPECT_EQ(refused(root, "nope", Flags::FlagSendRepresentation), Status::NotSupported);
}

TEST_F(IoserviceServer, OpenLookupFailures) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "nope", Flags::None), Status::NotFound);
    EXPECT_EQ(refused(root, "bin/nope", Flags::None), Status::NotFound);
    EXPECT_EQ(refused(root, "config/readme/more", Flags::None), Status::NotDir);
}

TEST_F(IoserviceServer, OpenMatchesTheProtocolAskedFor) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "bin", Flags::ProtocolFile), Status::NotFile);
    EXPECT_EQ(refused(root, "bin", Flags::ProtocolService), Status::BadType);
    EXPECT_EQ(refused(root, "config/readme", Flags::ProtocolDirectory), Status::NotDir);
    EXPECT_EQ(refused(root, "config/readme", Flags::ProtocolSymlink), Status::BadType);
    // Several protocols accept any of them, and none accepts what is there.
    auto const either = Flags::ProtocolDirectory | Flags::ProtocolFile;
    auto const bin = opened(root, "bin", either);
    EXPECT_EQ(held(m_server.node(bin)), m_bin);
    m_server.release(bin);
    auto const readme = opened(root, "config/readme", either | Flags::PermReadBytes);
    EXPECT_EQ(held(m_server.node(readme)), m_readme);
}

TEST_F(IoserviceServer, OpenRefusesFileFlagsThatDoNotApply) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "bin", Flags::FileAppend), Status::InvArgs);
    EXPECT_EQ(refused(root, "bin", Flags::FileTruncate), Status::InvArgs);
    // Truncating needs WRITE_BYTES in the same request.
    EXPECT_EQ(refused(root, "config/readme", Flags::PermReadBytes | Flags::FileTruncate),
              Status::InvArgs);
    // Append on a file that is never written changes nothing.
    opened(root, "config/readme", Flags::PermReadBytes | Flags::FileAppend);
}

// --- Rights ------------------------------------------------------------------

TEST_F(IoserviceServer, FileRightsShrinkToWhatTheFileProtocolUses) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    // PERM_READABLE also asks for CONNECT, ENUMERATE and TRAVERSE, which a
    // file has no use for: they are dropped, not refused (design D21).
    auto const readme = opened(root, "config/readme", ours_io::PERM_READABLE);
    EXPECT_EQ(held(m_server.rights(readme)), Operations::ReadBytes | Operations::GetAttributes);
    // A directory keeps all it asked for: they bound what is opened below.
    auto const bin = opened(root, "bin", ours_io::PERM_READABLE);
    EXPECT_EQ(held(m_server.rights(bin)), ours_io::R_STAR_DIR);
    // The inherit flags are cut and ignored: the connection gets exactly the
    // rights the request names.
    auto const config = opened(root, "config", ours_io::PERM_READABLE | Flags::PermInheritExecute);
    EXPECT_EQ(held(m_server.rights(config)), ours_io::R_STAR_DIR);
}

TEST_F(IoserviceServer, ExecuteNeedsAnExecutableFile) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    auto const rx = Flags::PermReadBytes | Flags::PermExecute;
    EXPECT_EQ(refused(root, "config/readme", rx), Status::AccessDenied);
    auto const x = opened(root, "bin/x", rx);
    EXPECT_EQ(held(m_server.rights(x)), Operations::ReadBytes | Operations::Execute);

    // Nor more than the connection holds: a directory opened without
    // EXECUTE is a ceiling for the files under it.
    auto const bin = opened(root, "bin", ours_io::PERM_READABLE);
    EXPECT_EQ(refused(bin, "x", rx), Status::AccessDenied);
    auto const readable = bind(m_root, ours_io::R_STAR_DIR);
    EXPECT_EQ(refused(readable, "bin/x", rx), Status::AccessDenied);
}

TEST_F(IoserviceServer, WritingIsRefusedOnFiles) {
    auto const root = bind(m_root, ours_io::RW_STAR_DIR);
    EXPECT_EQ(refused(root, "config/readme", Flags::PermWriteBytes), Status::AccessDenied);
}

// --- ReadDirents ---------------------------------------------------------------

TEST_F(IoserviceServer, ReadDirentsListsDotFirstThenNameOrder) {
    using ours_io::DirentType;
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    auto const listing = read_dirents(root, ours_io::MAX_BUF);
    EXPECT_EQ(listing.status, Status::Ok);
    EXPECT_EQ(listing.names, (std::vector<std::string>{".", "bin", "config"}));
    EXPECT_EQ(listing.types, (std::vector<DirentType>{DirentType::Directory, DirentType::Directory,
                                                      DirentType::Directory}));
    // The end is an empty Ok.
    auto const end = read_dirents(root, ours_io::MAX_BUF);
    EXPECT_EQ(end.status, Status::Ok);
    EXPECT_TRUE(end.names.empty());

    auto const bin = opened(root, "bin", ours_io::PERM_READABLE);
    EXPECT_EQ(read_dirents(bin, ours_io::MAX_BUF).types,
              (std::vector<DirentType>{DirentType::Directory, DirentType::File}));
    auto const svc = bind(m_svc, ours_io::R_STAR_DIR);
    auto const services = read_dirents(svc, ours_io::MAX_BUF);
    EXPECT_EQ(services.names, (std::vector<std::string>{".", "echo"}));
    EXPECT_EQ(services.types,
              (std::vector<DirentType>{DirentType::Directory, DirentType::Service}));
}

TEST_F(IoserviceServer, ReadDirentsBufferTooSmall) {
    // An entry is 10 bytes and its name: "." 11, "bin" 13, "config" 16.
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    EXPECT_EQ(read_dirents(root, 10).status, Status::BufferTooSmall);
    EXPECT_EQ(read_dirents(root, 0).status, Status::BufferTooSmall);
    EXPECT_EQ(read_dirents(root, 11).names, (std::vector<std::string>{"."}));
    EXPECT_EQ(read_dirents(root, 12).status, Status::BufferTooSmall);
    // Two would need 29: the one that does not fit is sent first next time.
    EXPECT_EQ(read_dirents(root, 28).names, (std::vector<std::string>{"bin"}));
    EXPECT_EQ(read_dirents(root, 15).status, Status::BufferTooSmall);
    EXPECT_EQ(read_dirents(root, 16).names, (std::vector<std::string>{"config"}));
    auto const end = read_dirents(root, 16);
    EXPECT_EQ(end.status, Status::Ok);
    EXPECT_TRUE(end.names.empty());
}

TEST_F(IoserviceServer, ReadDirentsNeedsEnumerate) {
    auto const root = bind(m_root, Operations::Traverse | Operations::Connect);
    EXPECT_EQ(read_dirents(root, ours_io::MAX_BUF).status, Status::AccessDenied);
}

TEST_F(IoserviceServer, ReadDirentsAboveTheLimitIsOutOfRange) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    EXPECT_EQ(read_dirents(root, ours_io::MAX_BUF + 1).status, Status::OutOfRange);
    EXPECT_EQ(read_dirents(root, ~u64{0}).status, Status::OutOfRange);
    // Nothing moved.
    EXPECT_EQ(read_dirents(root, ours_io::MAX_BUF).names.front(), ".");
}

// --- Read ----------------------------------------------------------------------

TEST_F(IoserviceServer, ReadMovesTheSeekToTheEnd) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    auto const readme = opened(root, "config/readme", ours_io::PERM_READABLE);
    auto const first = read(readme, 5);
    EXPECT_EQ(first.status, Status::Ok);
    EXPECT_EQ(text_of(first.data), "hello");
    EXPECT_EQ(text_of(read(readme, 100).data), ", world");
    // End of file is an Ok with no bytes.
    auto const end = read(readme, 10);
    EXPECT_EQ(end.status, Status::Ok);
    EXPECT_TRUE(end.data.empty());
}

TEST_F(IoserviceServer, ReadAboveTheTransferLimitIsOutOfRange) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    auto const readme = opened(root, "config/readme", ours_io::PERM_READABLE);
    EXPECT_EQ(read(readme, ours_io::MAX_TRANSFER_SIZE + 1).status, Status::OutOfRange);
    EXPECT_EQ(text_of(read(readme, ours_io::MAX_TRANSFER_SIZE).data), README);
}

TEST_F(IoserviceServer, ReadNeedsReadBytes) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    auto const readme = opened(root, "config/readme", Flags::PermGetAttributes);
    EXPECT_EQ(read(readme, 5).status, Status::BadHandle);
}

// --- GetBackingMemory --------------------------------------------------------

TEST_F(IoserviceServer, BackingMemoryRightsFollowTheFlags) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    auto const x = opened(root, "bin/x", Flags::PermReadBytes);
    auto const readable = Rights::Basic | Rights::Map | Rights::GetProperty | Rights::Read;

    auto const shared = get_backing_memory(x, VmoFlags::Read);
    EXPECT_EQ(shared.status, Status::Ok);
    EXPECT_EQ(shared.vmo, DUPLICATE);
    EXPECT_EQ(duplicates().handle, VMO_X);
    EXPECT_EQ(duplicates().rights, readable);

    // PRIVATE_CLONE is the same duplicate with the same rights (design D21).
    auto const private_clone = get_backing_memory(x, VmoFlags::Read | VmoFlags::PrivateClone);
    EXPECT_EQ(private_clone.status, Status::Ok);
    EXPECT_EQ(duplicates().calls, 2u);
    EXPECT_EQ(duplicates().rights, readable);
}

TEST_F(IoserviceServer, BackingMemoryExecuteNeedsTheConnectionsExecute) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    auto const executable =
        Rights::Basic | Rights::Map | Rights::GetProperty | Rights::Read | Rights::Execute;
    // The flags fsio's spawn sends for an executable (design D12).
    auto const spawn = VmoFlags::Read | VmoFlags::Execute | VmoFlags::PrivateClone;

    auto const rx = opened(root, "bin/x", Flags::PermReadBytes | Flags::PermExecute);
    EXPECT_EQ(get_backing_memory(rx, spawn).status, Status::Ok);
    EXPECT_EQ(duplicates().rights, executable);
    EXPECT_EQ(get_backing_memory(rx, VmoFlags::Read | VmoFlags::Execute).status, Status::Ok);
    EXPECT_EQ(duplicates().rights, executable);

    auto const r = opened(root, "bin/x", Flags::PermReadBytes);
    EXPECT_EQ(get_backing_memory(r, spawn).status, Status::AccessDenied);
    EXPECT_EQ(duplicates().calls, 2u);
}

TEST_F(IoserviceServer, BackingMemoryRefusals) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    auto const readme = opened(root, "config/readme", ours_io::PERM_READABLE);
    EXPECT_EQ(get_backing_memory(readme, VmoFlags::PrivateClone | VmoFlags::SharedBuffer).status,
              Status::InvArgs);
    EXPECT_EQ(get_backing_memory(readme, VmoFlags::Read | VmoFlags::Write).status,
              Status::AccessDenied);
    auto const attributes = opened(root, "config/readme", Flags::PermGetAttributes);
    EXPECT_EQ(get_backing_memory(attributes, VmoFlags::Read).status, Status::AccessDenied);
    EXPECT_EQ(duplicates().calls, 0u);
}

TEST_F(IoserviceServer, BackingMemoryPassesTheDuplicateFailureOn) {
    auto const root = bind(m_root, ours_io::RX_STAR_DIR);
    auto const readme = opened(root, "config/readme", ours_io::PERM_READABLE);
    duplicates().answer = Status::NoResource;
    auto const reply = get_backing_memory(readme, VmoFlags::Read);
    EXPECT_EQ(reply.status, Status::NoResource);
    EXPECT_EQ(reply.vmo, Uandle::Invalid);
}

// --- Clone and Close -----------------------------------------------------------

TEST_F(IoserviceServer, CloneKeepsTheRightsAndStartsOver) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    auto const readme = opened(root, "config/readme", ours_io::PERM_READABLE);
    EXPECT_EQ(text_of(read(readme, 5).data), "hello");

    auto const outcome = clone(readme, OTHER_OBJECT);
    ASSERT_EQ(outcome.action, Action::Bind);
    EXPECT_EQ(outcome.handle, OTHER_OBJECT);
    auto const copy = outcome.connection;
    EXPECT_EQ(held(m_server.channel(copy)), OTHER_OBJECT);
    EXPECT_EQ(held(m_server.rights(copy)), held(m_server.rights(readme)));
    EXPECT_EQ(text_of(read(copy, 5).data), "hello");
    EXPECT_EQ(text_of(read(readme, 2).data), ", ");

    // A directory's clone lists from "." again.
    EXPECT_EQ(read_dirents(root, ours_io::MAX_BUF).names.size(), 3u);
    m_server.release(copy);
    auto const root_copy = clone(root, OTHER_OBJECT);
    ASSERT_EQ(root_copy.action, Action::Bind);
    EXPECT_EQ(held(m_server.rights(root_copy.connection)), ours_io::R_STAR_DIR);
    EXPECT_EQ(read_dirents(root_copy.connection, ours_io::MAX_BUF).names.front(), ".");
}

TEST_F(IoserviceServer, CloseAnswersThenReleases) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    auto const outcome = dispatch(root, ours_io::encode_close(m_bytes, {.txid = TXID}));
    EXPECT_EQ(outcome.action, Action::ReplyAndClose);
    auto const reply = ours_io::decode_close_reply(outcome.message, TXID);
    ASSERT_TRUE(reply.is_ok());
    EXPECT_EQ(reply->status, Status::Ok);
    // The id is stale now, and anything sent on it is dropped.
    EXPECT_TRUE(m_server.channel(root).is_none());
    EXPECT_EQ(read_dirents_outcome(root), Action::Drop);
}

TEST_F(IoserviceServer, AFullTableRefusesWithNoResource) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    for (auto count = 1; count < 4; ++count) {
        bind(m_root, ours_io::R_STAR_DIR);
    }
    EXPECT_EQ(m_server.bind(CHANNEL, m_root, ours_io::R_STAR_DIR).unwrap_err(), Status::NoResource);
    EXPECT_EQ(refused(root, "bin", ours_io::PERM_READABLE), Status::NoResource);
    auto const cloned = clone(root, OBJECT);
    EXPECT_EQ(cloned.action, Action::Refuse);
    EXPECT_EQ(epitaph(cloned), Status::NoResource);

    // Releasing one frees its slot under a new generation: the old id stays
    // stale even though its slot serves again.
    m_server.release(root);
    auto const again = bind(m_root, ours_io::R_STAR_DIR);
    EXPECT_EQ(again.index, root.index);
    EXPECT_NE(again.generation, root.generation);
    EXPECT_TRUE(m_server.channel(root).is_none());
}

TEST_F(IoserviceServer, ARefusedOpenSaysWhatItAskedForAndWhy) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);

    // The status the epitaph carries, and the path as it was asked for.
    auto const missing = open(root, "bin/nope", Flags::None);
    ASSERT_EQ(missing.action, Action::Refuse);
    EXPECT_EQ(missing.status, Status::NotFound);
    EXPECT_EQ(epitaph(missing), missing.status);
    ASSERT_TRUE(missing.path.is_some());
    EXPECT_EQ(*missing.path, "bin/nope");

    // A path the server will not judge valid is still the path that was
    // asked for, and is passed on as it came.
    auto const invalid = open(root, "a//b", Flags::None);
    ASSERT_EQ(invalid.action, Action::Refuse);
    EXPECT_EQ(invalid.status, Status::BadPath);
    ASSERT_TRUE(invalid.path.is_some());
    EXPECT_EQ(*invalid.path, "a//b");

    // The ones refused by a check before the tree, too.
    auto const denied = open(root, "bin", Flags::PermWriteBytes);
    ASSERT_EQ(denied.action, Action::Refuse);
    EXPECT_EQ(denied.status, Status::AccessDenied);
    ASSERT_TRUE(denied.path.is_some());
    EXPECT_EQ(*denied.path, "bin");
}

TEST_F(IoserviceServer, ARefusedCloneAsksForNoPath) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    for (auto count = 1; count < 4; ++count) {
        bind(m_root, ours_io::R_STAR_DIR);
    }
    auto const cloned = clone(root, OBJECT);
    ASSERT_EQ(cloned.action, Action::Refuse);
    EXPECT_EQ(cloned.status, Status::NoResource);
    EXPECT_EQ(epitaph(cloned), cloned.status);
    EXPECT_TRUE(cloned.path.is_none());
}

// --- Services ------------------------------------------------------------------

TEST_F(IoserviceServer, OpeningAServiceHandsTheChannelBack) {
    auto const svc = bind(m_svc, ours_io::R_STAR_DIR);
    for (auto const flags: {Flags::ProtocolService, Flags::None}) {
        auto const outcome = open(svc, "echo", flags);
        EXPECT_EQ(outcome.action, Action::Connect);
        EXPECT_EQ(outcome.handle, OBJECT);
        EXPECT_EQ(outcome.service, ECHO);
    }
}

TEST_F(IoserviceServer, AServiceTakesNoOtherFlags) {
    auto const svc = bind(m_svc, ours_io::R_STAR_DIR);
    EXPECT_EQ(refused(svc, "echo", Flags::ProtocolService | Flags::PermConnect), Status::InvArgs);
    EXPECT_EQ(refused(svc, "echo", Flags::ProtocolService | static_cast<Flags>(u64{1} << 62)),
              Status::InvArgs);
    EXPECT_EQ(refused(svc, "echo", Flags::ProtocolFile), Status::NotFile);
    EXPECT_EQ(refused(svc, "echo", Flags::ProtocolDirectory | Flags::ProtocolService),
              Status::NotDir);
    EXPECT_EQ(refused(svc, "echo", Flags::ProtocolSymlink), Status::BadType);
}

TEST_F(IoserviceServer, AServiceNeedsConnectHoweverItIsAskedFor) {
    // Open's check 4 looks at CONNECT only when the flags name
    // PROTOCOL_SERVICE; with no flags, the service check must still refuse.
    auto const no_connect = bind(m_svc, Operations::Enumerate | Operations::Traverse);
    EXPECT_EQ(refused(no_connect, "echo", Flags::None), Status::AccessDenied);
}

// --- Malformed messages ------------------------------------------------------

TEST_F(IoserviceServer, MalformedMessagesDropTheConnection) {
    struct Case {
        char const *what;
        NodeId node;
        std::vector<u8> bytes;
        std::vector<Uandle> handles = {};
    };
    auto const bytes_of_message = [](ustl::Result<wire::Message, Status> const &encoded) {
        auto const message = encoded.unwrap();
        return std::vector<u8>(message.bytes.begin(), message.bytes.end());
    };

    auto const read_bytes =
        bytes_of_message(ours_io::encode_read(m_bytes, {.txid = TXID, .count = 1}));
    auto reply_bytes = read_bytes;
    reply_bytes[4] = u8{1}; // the direction: ToClient
    auto unknown_bytes = read_bytes;
    wire::store(std::span{unknown_bytes}, 8, u64{0x1234'5678});
    auto const open_bytes = bytes_of_message(ours_io::encode_open(
        m_bytes, m_handles, {.flags = Flags::None, .path = "x", .object = OBJECT}));
    auto const dirents_bytes =
        bytes_of_message(ours_io::encode_read_dirents(m_bytes, {.txid = TXID, .max_bytes = 100}));
    auto no_txid_bytes = dirents_bytes;
    wire::store(std::span{no_txid_bytes}, 0, u32{0});

    auto const cases = std::vector<Case>{
        {"a request cut short", m_readme, {read_bytes.begin(), read_bytes.end() - 1}},
        {"a reply sent as a request", m_readme, reply_bytes},
        {"an ordinal nobody defines", m_readme, unknown_bytes},
        {"a File method on a directory", m_root, read_bytes},
        {"a Directory method on a file", m_readme, open_bytes, {OBJECT}},
        {"an Open without its handle", m_root, open_bytes},
        {"a request with a handle it has no field for", m_root, dirents_bytes, {OBJECT}},
        {"a two-way request with txid 0", m_root, no_txid_bytes},
    };

    for (auto const &test: cases) {
        auto const root = bind(m_root, ours_io::RX_STAR_DIR | Operations::Execute);
        auto connection = root;
        if (test.node != m_root) {
            connection = opened(root, "config/readme", ours_io::PERM_READABLE);
        }
        auto const outcome = m_server.dispatch(
            connection, wire::Message{.bytes = test.bytes, .handles = test.handles}, m_out);
        EXPECT_EQ(outcome.action, Action::Drop) << test.what;
        EXPECT_TRUE(m_server.channel(connection).is_none()) << test.what;
        m_server.release(root);
    }
}

TEST_F(IoserviceServer, AStaleIdIsDropped) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    m_server.release(root);
    EXPECT_EQ(read_dirents_outcome(root), Action::Drop);
    EXPECT_EQ(read_dirents_outcome(ConnectionId{.index = 99, .generation = 0}), Action::Drop);
    // Releasing it again changes nothing.
    m_server.release(root);
    auto const next = bind(m_root, ours_io::R_STAR_DIR);
    EXPECT_EQ(next.generation, root.generation + 1);
}

TEST_F(IoserviceServer, GenerationZeroNamesNoConnection) {
    // A slot's first connection has generation 1, so an id nobody filled in,
    // {0, 0}, does not name the connection in slot 0 even while it is live.
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    EXPECT_EQ(root.index, 0U);
    EXPECT_EQ(root.generation, ioservice::FIRST_GENERATION);
    EXPECT_TRUE(m_server.channel(ConnectionId{}).is_none());
    EXPECT_EQ(read_dirents_outcome(ConnectionId{}), Action::Drop);
    // The Drop for the stale id left the live connection alone.
    EXPECT_TRUE(m_server.channel(root).is_some());
}

// --- bind ------------------------------------------------------------------------

TEST_F(IoserviceServer, BindRefusals) {
    EXPECT_EQ(m_server.bind(Uandle::Invalid, m_root, ours_io::R_STAR_DIR).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(m_server.bind(CHANNEL, NodeId{15}, ours_io::R_STAR_DIR).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(m_server.bind(CHANNEL, m_root, static_cast<Operations>(u64{0x200})).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(m_server.bind(CHANNEL, m_readme, ours_io::R_STAR_DIR).unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_server.bind(CHANNEL, m_echo, ours_io::R_STAR_DIR).unwrap_err(), Status::NotDir);
    // A remote entry is not one of this server's directories either: what it
    // stands for is another server's to bind.
    EXPECT_EQ(m_server.bind(CHANNEL, m_boot, ours_io::R_STAR_DIR).unwrap_err(), Status::NotDir);
}

// --- Open: forwarding through a remote entry --------------------------------------

TEST_F(IoserviceServer, OpenThroughARemoteEntryForwardsTheRestOfThePath) {
    auto const mount = bind(m_mount, ours_io::RX_STAR_DIR);
    auto const outcome =
        open(mount, "mnt/boot/config/readme", Flags::PermReadBytes | Flags::PermGetAttributes);
    EXPECT_EQ(outcome.action, Action::Forward);
    EXPECT_EQ(outcome.forward_channel, REMOTE);
    auto const forwarded = ours_io::decode_open(outcome.message);
    ASSERT_TRUE(forwarded.is_ok());
    EXPECT_EQ(forwarded->path, "config/readme");
    EXPECT_EQ(forwarded->flags, Flags::PermReadBytes | Flags::PermGetAttributes);
    EXPECT_EQ(forwarded->object, OBJECT);
}

TEST_F(IoserviceServer, OpenOfTheMountPointItselfForwardsDot) {
    auto const mount = bind(m_mount, ours_io::RX_STAR_DIR);
    auto const outcome = open(mount, "mnt/boot", Flags::PermReadBytes);
    EXPECT_EQ(outcome.action, Action::Forward);
    EXPECT_EQ(outcome.forward_channel, REMOTE);
    auto const forwarded = ours_io::decode_open(outcome.message);
    ASSERT_TRUE(forwarded.is_ok());
    EXPECT_EQ(forwarded->path, ".");
    EXPECT_EQ(forwarded->object, OBJECT);
}

TEST_F(IoserviceServer, OpenThroughARemoteEntryCarriesCreationFlags) {
    // A local target refuses to create (OpenRefusesToCreate); a remote one
    // is for the server that owns the subtree to judge, so the flags pass
    // through unchanged.
    auto const mount = bind(m_mount, ours_io::RW_STAR_DIR);
    auto const flags = ours_io::PERM_WRITABLE | Flags::FlagMaybeCreate;
    auto const outcome = open(mount, "mnt/boot/new", flags);
    EXPECT_EQ(outcome.action, Action::Forward);
    auto const forwarded = ours_io::decode_open(outcome.message);
    ASSERT_TRUE(forwarded.is_ok());
    EXPECT_EQ(forwarded->flags, flags);
}

TEST_F(IoserviceServer, OpenThroughARemoteEntryStillChecksRightsFirst) {
    // Check 3 runs before anything is forwarded: a mount point is no way
    // around a connection's ceiling.
    auto const mount = bind(m_mount, ours_io::R_STAR_DIR);
    EXPECT_EQ(refused(mount, "mnt/boot/new", Flags::PermWriteBytes), Status::AccessDenied);
}

TEST_F(IoserviceServer, LocalCreationIsStillRefusedBesideTheMount) {
    auto const mount = bind(m_mount, ours_io::RW_STAR_DIR);
    EXPECT_EQ(refused(mount, "mnt/new", ours_io::PERM_WRITABLE | Flags::FlagMaybeCreate),
              Status::NotSupported);
}

TEST_F(IoserviceServer, AMountPointListsAsADirectory) {
    auto const mount = bind(m_mount, ours_io::R_STAR_DIR);
    auto const mnt = opened(mount, "mnt", Flags::PermEnumerate);
    auto const listing = read_dirents(mnt, ours_io::MAX_BUF);
    EXPECT_EQ(listing.status, Status::Ok);
    // "." first, then the mount point standing in for the directory the
    // remote serves.
    ASSERT_EQ(listing.names.size(), 2U);
    EXPECT_EQ(listing.names[0], ".");
    EXPECT_EQ(listing.names[1], "boot");
    EXPECT_EQ(listing.types[1], ours_io::DirentType::Directory);
}
// --- Subtree nodes -------------------------------------------------------------------

/// The tree of IoserviceServer, with a place the program serves itself:
///
///   root/mnt/             a directory
///     fs                  a Subtree node, FS
///     note                a file
class IoserviceServerSubtree: public IoserviceServer {
  protected:
    /// What the node was added with.
    static constexpr auto FS = u64{21};

    IoserviceServerSubtree() {
        m_mnt = m_tree.add_directory(m_root, "mnt").unwrap();
        m_fs = m_tree.add_subtree(m_mnt, "fs", FS).unwrap();
        m_note = m_tree
                     .add_file(m_mnt, "note",
                               {.vmo = VMO_README, .bytes = bytes_of(README), .executable = false})
                     .unwrap();
    }

    /// An Open that must reach the program: its Outcome, checked for the parts
    /// every such Open shares.
    auto handed_over(ConnectionId const connection, std::string_view const path, Flags const flags)
        -> Outcome {
        auto const outcome = open(connection, path, flags, OTHER_OBJECT);
        EXPECT_EQ(outcome.action, Action::Subtree) << path;
        // The Open's own channel, not the connection's.
        EXPECT_EQ(outcome.handle, OTHER_OBJECT) << path;
        EXPECT_EQ(outcome.service, FS) << path;
        return outcome;
    }

    NodeId m_mnt{};
    NodeId m_fs{};
    NodeId m_note{};
};

TEST_F(IoserviceServerSubtree, AnOpenThatWalksToASubtreeNodeIsHandedToTheProgram) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);

    // The value, the rest of the path after the node, and the channel; and
    // the node itself is where the walk ended, so "." is its rest.
    EXPECT_EQ(handed_over(root, "mnt/fs", ours_io::PERM_READABLE).rest, ".");
    EXPECT_EQ(handed_over(root, "mnt/fs/a", ours_io::PERM_READABLE).rest, "a");
    EXPECT_EQ(handed_over(root, "mnt/fs/a/b/c", ours_io::PERM_READABLE).rest, "a/b/c");

    // From a connection further down, the path starts there.
    auto const mnt = opened(root, "mnt", Flags::ProtocolDirectory | ours_io::PERM_READABLE);
    EXPECT_EQ(handed_over(mnt, "fs/x/y", ours_io::PERM_READABLE).rest, "x/y");
    EXPECT_EQ(handed_over(mnt, "fs", ours_io::PERM_READABLE).rest, ".");
}

TEST_F(IoserviceServerSubtree, TheProgramGetsTheFlagsAsTheClientSentThem) {
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    // Whatever the client asked for, within what the connection holds, is
    // passed on whole: the protocol bits the node would not judge, the file
    // flags that a directory refuses, a bit nobody defines. The flags are the
    // program's tree's to judge, for what it is.
    for (auto const flags: {
             Flags::None,
             ours_io::PERM_READABLE,
             Flags::ProtocolDirectory | ours_io::PERM_READABLE,
             Flags::ProtocolFile | Flags::PermReadBytes,
             Flags::ProtocolSymlink,
             Flags::ProtocolService,
             Flags::PermReadBytes | Flags::PermInheritWrite | Flags::PermInheritExecute,
             Flags::ProtocolDirectory | Flags::FileAppend | Flags::FileTruncate,
             Flags::PermTraverse | static_cast<Flags>(u64{1} << 62),
         }) {
        EXPECT_EQ(handed_over(root, "mnt/fs/x", flags).flags, flags) << std::to_underlying(flags);
    }
}

TEST_F(IoserviceServerSubtree, AnOpenOfASubtreeNodeMakesNoConnection) {
    // Four slots, one taken by |root|. Were an Open to take one for the
    // channel it hands over, the fourth would find the table full.
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    for (auto count = 0; count < 12; ++count) {
        EXPECT_EQ(handed_over(root, "mnt/fs/x", ours_io::PERM_READABLE).action, Action::Subtree);
    }
    bind(m_root, ours_io::R_STAR_DIR);
    bind(m_root, ours_io::R_STAR_DIR);
    bind(m_root, ours_io::R_STAR_DIR);
    // The table is now full of connections that were bound, and an Open of a
    // directory finds it so; the Subtree Open still reaches the program,
    // since it asks for no slot.
    EXPECT_EQ(refused(root, "mnt", ours_io::PERM_READABLE), Status::NoResource);
    EXPECT_EQ(handed_over(root, "mnt/fs/x", ours_io::PERM_READABLE).action, Action::Subtree);
}

TEST_F(IoserviceServerSubtree, TheConnectionsOwnChecksRunBeforeTheProgramIsAsked) {
    // Each case is an Open the program must never see: the connection's
    // checks of Open, in their order, which hold the Open whether it is for a
    // node of this tree or for one below a Subtree node.
    struct Case {
        char const *what;
        Operations held;
        char const *path;
        Flags flags;
        Status expected;
    };
    constexpr auto SERVED = ours_io::PERM_READABLE;
    for (auto const &c: {
             // 1. The path's syntax, whatever the node would have been.
             Case{"a leading slash", ours_io::R_STAR_DIR, "/mnt/fs/x", SERVED, Status::BadPath},
             Case{"a dot-dot after the node", ours_io::R_STAR_DIR, "mnt/fs/../x", SERVED,
                  Status::BadPath},
             Case{"an empty segment after the node", ours_io::R_STAR_DIR, "mnt/fs//x", SERVED,
                  Status::BadPath},
             Case{"a trailing slash", ours_io::R_STAR_DIR, "mnt/fs/", SERVED, Status::BadPath},
             Case{"an empty path", ours_io::R_STAR_DIR, "", SERVED, Status::BadPath},
             // 2. Going below the directory needs TRAVERSE.
             Case{"no traverse",
                  Operations::Connect | Operations::Enumerate | Operations::ReadBytes, "mnt/fs/x",
                  Flags::PermReadBytes, Status::AccessDenied},
             // 3. Rights are a subset of the connection's.
             Case{"write on a reader", ours_io::R_STAR_DIR, "mnt/fs/x",
                  Flags::PermReadBytes | Flags::PermWriteBytes, Status::AccessDenied},
             Case{"modify on a reader", ours_io::R_STAR_DIR, "mnt/fs/x", Flags::PermModifyDirectory,
                  Status::AccessDenied},
             Case{"execute on a reader", ours_io::R_STAR_DIR, "mnt/fs", Flags::PermExecute,
                  Status::AccessDenied},
             // 4. Connecting needs CONNECT.
             Case{"service without connect",
                  Operations::Enumerate | Operations::Traverse | Operations::ReadBytes, "mnt/fs/x",
                  Flags::ProtocolService, Status::AccessDenied},
             // 6. Creating, and what the design cut.
             Case{"create without modify", ours_io::R_STAR_DIR, "mnt/fs/new",
                  Flags::FlagMaybeCreate | Flags::ProtocolFile, Status::AccessDenied},
             Case{"create with modify", ours_io::R_STAR_DIR | Operations::ModifyDirectory,
                  "mnt/fs/new", Flags::FlagMustCreate | Flags::ProtocolFile | Flags::PermReadBytes,
                  Status::NotSupported},
             Case{"a node-only connection", ours_io::R_STAR_DIR, "mnt/fs/x", Flags::ProtocolNode,
                  Status::NotSupported},
             Case{"a representation event", ours_io::R_STAR_DIR, "mnt/fs",
                  Flags::FlagSendRepresentation, Status::NotSupported},
             // 8. The walk to the node.
             Case{"a name missing before the node", ours_io::R_STAR_DIR, "nope/fs/x", SERVED,
                  Status::NotFound},
             Case{"a name missing next to the node", ours_io::R_STAR_DIR, "mnt/nope/x", SERVED,
                  Status::NotFound},
             Case{"a file before the node", ours_io::R_STAR_DIR, "mnt/note/fs/x", SERVED,
                  Status::NotDir},
         }) {
        auto const connection = bind(m_root, c.held);
        EXPECT_EQ(refused(connection, c.path, c.flags), c.expected) << c.what;
        m_server.release(connection);
    }
}

TEST_F(IoserviceServerSubtree, ARefusalIsReportedForTheWholePathAsAskedFor) {
    // The Open that never reached the program says what it asked for, as any
    // refused Open does.
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    auto const outcome = open(root, "mnt/fs/x", Flags::PermWriteBytes);
    ASSERT_EQ(outcome.action, Action::Refuse);
    EXPECT_EQ(outcome.status, Status::AccessDenied);
    ASSERT_TRUE(outcome.path.is_some());
    EXPECT_EQ(*outcome.path, "mnt/fs/x");
}

TEST_F(IoserviceServerSubtree, ASubtreeNodeIsListedAsADirectory) {
    auto const mnt = bind(m_mnt, ours_io::R_STAR_DIR);
    auto const listing = read_dirents(mnt, ours_io::MAX_BUF);
    EXPECT_EQ(listing.status, Status::Ok);
    EXPECT_EQ(listing.names, (std::vector<std::string>{".", "fs", "note"}));
    EXPECT_EQ(listing.types, (std::vector<ours_io::DirentType>{ours_io::DirentType::Directory,
                                                               ours_io::DirentType::Directory,
                                                               ours_io::DirentType::File}));
}

TEST_F(IoserviceServerSubtree, NoConnectionIsBoundToASubtreeNode) {
    EXPECT_EQ(m_server.bind(CHANNEL, m_fs, ours_io::R_STAR_DIR).unwrap_err(), Status::NotDir);
}

TEST_F(IoserviceServerSubtree, ThePathOfAnOpenThatMissesTheNodeIsNotHandedOver) {
    // A sibling of the node, and a file next to it, are this tree's own.
    auto const root = bind(m_root, ours_io::R_STAR_DIR);
    auto const note = open(root, "mnt/note", Flags::ProtocolFile | Flags::PermReadBytes);
    EXPECT_EQ(note.action, Action::Bind);
    auto const mnt = open(root, "mnt", Flags::ProtocolDirectory | ours_io::PERM_READABLE);
    EXPECT_EQ(mnt.action, Action::Bind);
}
} // namespace
