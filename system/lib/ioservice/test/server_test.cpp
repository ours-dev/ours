/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// ioservice::Server (<ioservice/server.hpp>), driven the way the root will drive it:
// one encoded ours.io message at a time, with the Outcome read back as a
// program would act on it. No channel exists; handles are numbers, and the
// one system call the server makes is a stand-in that records what it was
// asked.
#include <ioservice/server.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <prot/io.hpp>
#include <wire/wire.hpp>

namespace {
// The size of a connection, which the Outgoing::Storage of every provider
// holds 32 of (docs/repairs/261103/ioservice.md, item 1). A change that moves
// it updates the figure there and in the README with it.
static_assert(sizeof(ioservice::Connection) == 80);

using ioservice::Action;
using ioservice::Answer;
using ioservice::ConnectionId;
using ioservice::Done;
using ioservice::Entries;
using ioservice::EntriesResult;
using ioservice::NodeId;
using ioservice::ObjectKind;
using ioservice::Opened;
using ioservice::OpenResult;
using ioservice::Outcome;
using ioservice::Pending;
using ioservice::ReadResult;
using ioservice::Ticket;
using ioservice::VmoResult;
using ours::Rights;
using ours::Status;
using ours::Uandle;
using prot::io::Flags;
using prot::io::Operations;
using prot::io::VmoFlags;

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
    std::vector<prot::io::DirentType> types;
};

/// A record of the calls a fake received. Indexing past its end is a test
/// failure and a default value, not undefined behaviour: code that makes fewer
/// calls than a test expects must fail that test, not end the whole binary.
template <typename T>
class Log {
  public:
    template <typename... Args>
    auto emplace_back(Args &&...args) -> void {
        m_items.emplace_back(std::forward<Args>(args)...);
    }

    auto push_back(T value) -> void {
        m_items.push_back(std::move(value));
    }

    [[nodiscard]]
    auto size() const -> usize {
        return m_items.size();
    }

    [[nodiscard]]
    auto empty() const -> bool {
        return m_items.empty();
    }

    [[nodiscard]]
    auto begin() const -> typename std::vector<T>::const_iterator {
        return m_items.begin();
    }

    [[nodiscard]]
    auto end() const -> typename std::vector<T>::const_iterator {
        return m_items.end();
    }

    [[nodiscard]]
    auto operator[](usize const index) const -> T const & {
        if (index >= m_items.size()) {
            ADD_FAILURE() << "call " << index << " of " << m_items.size() << " was not made";
            return m_missing;
        }
        return m_items[index];
    }

    [[nodiscard]]
    auto back() const -> T const & {
        if (m_items.empty()) {
            ADD_FAILURE() << "no call was made";
            return m_missing;
        }
        return m_items.back();
    }

  private:
    std::vector<T> m_items;
    T m_missing{};
};

/// How the fake handler misbehaves, when a test wants it to.
enum class Fault : u8 {
    None,
    /// A Read returns one byte more than it was asked for.
    ReadsTooMuch,
    /// A ReadDirents returns one byte more than it was given room for.
    ListsTooMuch,
    /// A GetBackingMemory answers Ok with a handle that is not one.
    ReturnsAnInvalidVmo,
    /// An operation answers Pending with a ticket it was not given.
    PromisesAnotherTicket,
    /// An operation completes its own ticket before it returns Pending.
    CompletesInsideTheCall,
};

/// The operations a FakeFs may answer Pending, as a mask.
constexpr auto PEND_OPEN = u8{1};
constexpr auto PEND_READ = u8{2};
constexpr auto PEND_DIRENTS = u8{4};
constexpr auto PEND_VMO = u8{8};
constexpr auto PEND_ALL = u8{15};

/// A NodeHandler serving a small tree of its own,
///
///   /                     a directory
///     bin/
///       x                 an executable file, X_CONTENTS
///     config/
///       readme            a file, README
///
/// the content IoserviceServer's tree holds, so that the replies of the two can
/// be compared. It keeps what it was asked and what it holds open, so that a
/// test can say what the Server told it and when. It answers Done unless the
/// operation is in |pend|, and then it parks the result it would have answered
/// with: the test ends the request with complete(), when it likes.
class FakeFs final: public ioservice::NodeHandler {
  public:
    struct Entry {
        std::string_view path;
        ObjectKind kind;
        std::string_view bytes;
        bool executable;
    };

    static constexpr auto ENTRIES = std::array<Entry, 5>{
        Entry{"", ObjectKind::Directory, {}, false},
        Entry{"bin", ObjectKind::Directory, {}, false},
        Entry{"bin/x", ObjectKind::File, X_CONTENTS, true},
        Entry{"config", ObjectKind::Directory, {}, false},
        Entry{"config/readme", ObjectKind::File, README, false},
    };

    struct OpenCall {
        u64 value;
        ustl::Option<u64> directory;
        std::string path;
        Flags flags;
        Ticket ticket;
    };

    struct ReadCall {
        u64 value;
        u64 cookie;
        u64 offset;
        u64 count;
        Ticket ticket;
    };

    struct DirentsCall {
        u64 value;
        u64 cookie;
        u64 cursor;
        u64 max_bytes;
        Ticket ticket;
    };

    struct VmoCall {
        u64 value;
        u64 cookie;
        VmoFlags flags;
        Ticket ticket;
    };

    /// A result held back for a request answered Pending.
    struct Parked {
        Ticket ticket;
        std::variant<OpenResult, ReadResult, EntriesResult, VmoResult> result;
    };

    auto open(u64 const value, ustl::Option<u64> const directory, std::string_view const path,
              Flags const flags, Ticket const ticket) -> Answer<OpenResult> override {
        opens.push_back({value, directory, std::string{path}, flags, ticket});
        return answer(PEND_OPEN, ticket, open_entry(directory, path, flags));
    }

    auto read(u64 const value, u64 const cookie, u64 const offset, u64 const count,
              Ticket const ticket) -> Answer<ReadResult> override {
        reads.push_back({value, cookie, offset, count, ticket});
        return answer(PEND_READ, ticket, read_file(cookie, offset, count));
    }

    auto read_dirents(u64 const value, u64 const cookie, u64 const cursor, u64 const max_bytes,
                      Ticket const ticket) -> Answer<EntriesResult> override {
        listings.push_back({value, cookie, cursor, max_bytes, ticket});
        return answer(PEND_DIRENTS, ticket, list(cookie, cursor, max_bytes));
    }

    auto get_backing_memory(u64 const value, u64 const cookie, VmoFlags const flags,
                            Ticket const ticket) -> Answer<VmoResult> override {
        vmos.push_back({value, cookie, flags, ticket});
        auto result = VmoResult{ustl::ok(DUPLICATE)};
        if (!live.contains(cookie)) {
            result = ustl::err(Status::BadHandle);
        } else if (fault == Fault::ReturnsAnInvalidVmo) {
            result = ustl::ok(Uandle::Invalid);
        }
        return answer(PEND_VMO, ticket, std::move(result));
    }

    auto clone(u64 const value, u64 const cookie) -> ustl::Result<u64, Status> override {
        clones.emplace_back(value, cookie);
        if (clone_error != Status::Ok) {
            return ustl::err(clone_error);
        }
        auto const found = live.find(cookie);
        if (found == live.end()) {
            ++bad_calls;
            return ustl::err(Status::BadHandle);
        }
        auto const copy = m_next_cookie++;
        live[copy] = found->second;
        return ustl::ok(copy);
    }

    auto release(u64 const value, u64 const cookie) -> void override {
        releases.emplace_back(value, cookie);
        if (live.erase(cookie) == 0) {
            ++bad_releases;
        }
    }

    /// Ends the parked request of |ticket| with the result it was parked with,
    /// through the Server's complete() for its operation.
    auto complete(ioservice::Server &server, ioservice::ReplyBuffer &out, Ticket const ticket)
        -> ustl::Result<Outcome, Status> {
        for (auto index = usize{0}; index < parked.size(); ++index) {
            if (parked[index].ticket != ticket) {
                continue;
            }
            auto const result = parked[index].result;
            parked.erase(parked.begin() + static_cast<std::ptrdiff_t>(index));
            // get_if, not visit: a visit may throw bad_variant_access, which
            // this build has no way to.
            if (auto const *const open = std::get_if<OpenResult>(&result)) {
                return server.complete(ticket, *open, out);
            }
            if (auto const *const read = std::get_if<ReadResult>(&result)) {
                return server.complete(ticket, *read, out);
            }
            if (auto const *const listing = std::get_if<EntriesResult>(&result)) {
                return server.complete(ticket, *listing, out);
            }
            return server.complete(ticket, *std::get_if<VmoResult>(&result), out);
        }
        ADD_FAILURE() << "no parked request for the ticket";
        return ustl::err(Status::NotFound);
    }

    // What a test sets.
    u8 pend = 0;
    Fault fault = Fault::None;
    /// A Read the handler fails with this status when not Ok.
    Status read_error = Status::Ok;
    Status clone_error = Status::Ok;
    /// For Fault::CompletesInsideTheCall: where the call completes its own
    /// ticket.
    ioservice::Server *server = nullptr;
    ioservice::ReplyBuffer *out = nullptr;

    // What a test reads back.
    Log<OpenCall> opens;
    Log<ReadCall> reads;
    Log<DirentsCall> listings;
    Log<VmoCall> vmos;
    Log<std::pair<u64, u64>> clones;
    Log<std::pair<u64, u64>> releases;
    std::vector<Parked> parked;
    /// The cookies handed out and not released yet, and the entry each names.
    std::map<u64, usize> live;
    /// Releases of a cookie that was not held, and calls on one.
    u32 bad_releases = 0;
    u32 bad_calls = 0;
    /// What complete() answered when Fault::CompletesInsideTheCall made the
    /// call try it.
    Status completed_inside = Status::Ok;
    /// The cookies of the Opens that succeeded, in order.
    Log<u64> cookies;

  private:
    template <typename R>
    auto answer(u8 const op, Ticket const ticket, R result) -> Answer<R> {
        if ((pend & op) == 0) {
            return Done<R>{std::move(result)};
        }
        if (fault == Fault::CompletesInsideTheCall) {
            // The ticket is good only once the call has returned Pending.
            auto const inside = server->complete(ticket, result, *out);
            completed_inside = Status::Ok;
            if (inside.is_err()) {
                completed_inside = inside.unwrap_err();
            }
        }
        parked.push_back({ticket, result});
        if (fault == Fault::PromisesAnotherTicket) {
            return Pending{Ticket{.connection = ticket.connection, .serial = ticket.serial + 100}};
        }
        return Pending{ticket};
    }

    auto open_entry(ustl::Option<u64> const directory, std::string_view const path,
                    Flags const flags) -> OpenResult {
        auto full = std::string{};
        if (directory.is_some()) {
            auto const found = live.find(*directory);
            if (found == live.end()) {
                ++bad_calls;
                return ustl::err(Status::BadHandle);
            }
            full = std::string{ENTRIES[found->second].path};
        }
        if (path != ".") {
            if (!full.empty()) {
                full += '/';
            }
            full += path;
        }
        for (auto index = usize{0}; index < ENTRIES.size(); ++index) {
            auto const &entry = ENTRIES[index];
            if (entry.path != full) {
                continue;
            }
            // EXECUTE on a file that is not executable is the handler's to
            // refuse: the Server cannot tell.
            if (entry.kind == ObjectKind::File && !entry.executable &&
                (flags & Flags::PermExecute) != Flags::None) {
                return ustl::err(Status::AccessDenied);
            }
            auto const cookie = m_next_cookie++;
            live[cookie] = index;
            cookies.push_back(cookie);
            return ustl::ok(Opened{.kind = entry.kind, .cookie = cookie});
        }
        return ustl::err(Status::NotFound);
    }

    auto read_file(u64 const cookie, u64 const offset, u64 const count) -> ReadResult {
        if (read_error != Status::Ok) {
            return ustl::err(read_error);
        }
        auto const found = live.find(cookie);
        if (found == live.end()) {
            ++bad_calls;
            return ustl::err(Status::BadHandle);
        }
        if (fault == Fault::ReadsTooMuch) {
            return ustl::ok(std::span<u8 const>{TOO_MUCH}.first(count + 1));
        }
        auto const bytes = bytes_of(ENTRIES[found->second].bytes);
        auto const from = std::min<u64>(offset, bytes.size());
        auto const size = std::min<u64>(count, bytes.size() - from);
        return ustl::ok(bytes.subspan(from, size));
    }

    /// The entries directly below the directory at |path|, in order.
    static auto children_of(std::string_view const path) -> std::vector<usize> {
        auto children = std::vector<usize>{};
        for (auto index = usize{0}; index < ENTRIES.size(); ++index) {
            auto const entry = ENTRIES[index].path;
            if (entry.empty() || entry == path) {
                continue;
            }
            auto rest = entry;
            if (!path.empty()) {
                if (!entry.starts_with(path) || entry.size() <= path.size() + 1 ||
                    entry[path.size()] != '/') {
                    continue;
                }
                rest = entry.substr(path.size() + 1);
            }
            if (rest.find('/') == std::string_view::npos) {
                children.push_back(index);
            }
        }
        return children;
    }

    auto list(u64 const cookie, u64 const cursor, u64 const max_bytes) -> EntriesResult {
        auto const found = live.find(cookie);
        if (found == live.end()) {
            ++bad_calls;
            return ustl::err(Status::BadHandle);
        }
        // One byte more than the room, for a handler that overflows it. The
        // buffer outlives the call: a deque does not move what it holds.
        auto &buffer = m_buffers.emplace_back(max_bytes + 1, u8{0});
        if (fault == Fault::ListsTooMuch) {
            return ustl::ok(Entries{.dirents = std::span<u8 const>{buffer}, .next = cursor});
        }
        auto const children = children_of(ENTRIES[found->second].path);
        auto writer = prot::io::DirentWriter(std::span<u8>{buffer}.first(max_bytes));
        auto next = cursor;
        while (next < children.size()) {
            auto const &entry = ENTRIES[children[next]];
            auto const name = entry.path.substr(entry.path.rfind('/') + 1);
            auto type = prot::io::DirentType::File;
            if (entry.kind == ObjectKind::Directory) {
                type = prot::io::DirentType::Directory;
            }
            if (writer.append({.ino = prot::io::INO_UNKNOWN, .type = type, .name = name}) !=
                Status::Ok) {
                break;
            }
            ++next;
        }
        // The first entry does not fit.
        if (next == cursor && next < children.size()) {
            return ustl::err(Status::BufferTooSmall);
        }
        return ustl::ok(Entries{.dirents = writer.written(), .next = next});
    }

    static constexpr auto TOO_MUCH = std::array<u8, 5000>{};

    u64 m_next_cookie = 100;
    std::deque<std::vector<u8>> m_buffers;
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
    /// A fixture whose table holds |connections| connections: four, enough for
    /// the tests that fill it, unless a derived fixture needs more.
    explicit IoserviceServer(usize const connections = 4) : m_connections(connections) {
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
        return settle(m_server.dispatch(connection, message.unwrap(), m_out));
    }

    /// What a test does with the Outcome of a dispatch before it reads it. A
    /// fixture whose handler answers Pending ends the request here, so that
    /// every helper below reads the same replies whether the handler waited or
    /// not.
    virtual auto settle(Outcome const outcome) -> Outcome {
        return outcome;
    }

    auto open(ConnectionId const connection, std::string_view const path, Flags const flags,
              Uandle const object = OBJECT) -> Outcome {
        return dispatch(connection,
                        prot::io::encode_open(m_bytes, m_handles,
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

    auto read(ConnectionId const connection, u64 const count) -> prot::io::ReadReply {
        auto const outcome =
            dispatch(connection, prot::io::encode_read(m_bytes, {.txid = TXID, .count = count}));
        EXPECT_EQ(outcome.action, Action::Reply);
        auto const reply = prot::io::decode_read_reply(outcome.message, TXID);
        EXPECT_TRUE(reply.is_ok());
        if (reply.is_err()) {
            return {.status = Status::InternalError};
        }
        return reply.unwrap();
    }

    auto read_dirents(ConnectionId const connection, u64 const max_bytes) -> Listing {
        auto const outcome = dispatch(
            connection,
            prot::io::encode_read_dirents(m_bytes, {.txid = TXID, .max_bytes = max_bytes}));
        EXPECT_EQ(outcome.action, Action::Reply);
        auto const reply = prot::io::decode_read_dirents_reply(outcome.message, TXID);
        EXPECT_TRUE(reply.is_ok());
        if (reply.is_err()) {
            return {.status = Status::InternalError, .names = {}, .types = {}};
        }
        auto listing = Listing{.status = reply->status, .names = {}, .types = {}};
        auto reader = prot::io::DirentReader(reply->dirents);
        for (auto next = reader.next(); next.is_ok() && next->is_some(); next = reader.next()) {
            auto const &dirent = next.unwrap().value();
            EXPECT_EQ(dirent.ino, prot::io::INO_UNKNOWN);
            listing.names.emplace_back(dirent.name);
            listing.types.push_back(dirent.type);
        }
        return listing;
    }

    auto get_backing_memory(ConnectionId const connection, VmoFlags const flags)
        -> prot::io::GetBackingMemoryReply {
        auto const outcome =
            dispatch(connection,
                     prot::io::encode_get_backing_memory(m_bytes, {.txid = TXID, .flags = flags}));
        EXPECT_EQ(outcome.action, Action::Reply);
        auto const reply = prot::io::decode_get_backing_memory_reply(outcome.message, TXID);
        EXPECT_TRUE(reply.is_ok());
        if (reply.is_err()) {
            return {.status = Status::InternalError};
        }
        return reply.unwrap();
    }

    /// What a ReadDirents on |connection| comes to, for an id that may be
    /// stale.
    auto read_dirents_outcome(ConnectionId const connection) -> Action {
        return dispatch(connection, prot::io::encode_read_dirents(
                                        m_bytes, {.txid = TXID, .max_bytes = prot::io::MAX_BUF}))
            .action;
    }

    auto clone(ConnectionId const connection, Uandle const request) -> Outcome {
        return dispatch(connection,
                        prot::io::encode_clone(m_bytes, m_handles, {.request = request}));
    }

    template <typename T>
    static auto held(ustl::Option<T> const &option) -> T {
        EXPECT_TRUE(option.is_some());
        if (option.is_none()) {
            return T{};
        }
        return *option;
    }

    std::array<ioservice::Node, 24> m_nodes{};
    ioservice::Tree m_tree{m_nodes};
    std::vector<ioservice::Connection> m_connections;
    FakeFs m_fs{};
    ioservice::Server m_server{m_tree, m_connections, {.handle_duplicate = fake_duplicate}, &m_fs};
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
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "nope", Flags::PermWriteBytes), Status::AccessDenied);
    EXPECT_EQ(refused(root, "bin", Flags::PermModifyDirectory), Status::AccessDenied);
    EXPECT_EQ(refused(root, "nope", prot::io::PERM_READABLE), Status::NotFound);
}

TEST_F(IoserviceServer, OpenChecksConnectForProtocolService) {
    auto const no_connect = bind(m_svc, Operations::Enumerate | Operations::Traverse);
    EXPECT_EQ(refused(no_connect, "echo", Flags::ProtocolService), Status::AccessDenied);
    // Before the lookup: a missing name answers the same.
    EXPECT_EQ(refused(no_connect, "nope", Flags::ProtocolService), Status::AccessDenied);

    auto const svc = bind(m_svc, prot::io::R_STAR_DIR);
    EXPECT_EQ(refused(svc, "nope", Flags::ProtocolService), Status::NotFound);
}

TEST_F(IoserviceServer, OpenRefusesToCreate) {
    // MODIFY_DIRECTORY first. With it the answer is still NotSupported,
    // before the tree is looked at.
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "new", Flags::FlagMaybeCreate | Flags::ProtocolFile),
              Status::AccessDenied);

    auto const writable = bind(m_root, prot::io::RX_STAR_DIR | Operations::ModifyDirectory);
    EXPECT_EQ(refused(writable, "new", Flags::FlagMaybeCreate | Flags::ProtocolFile),
              Status::NotSupported);
    EXPECT_EQ(refused(writable, "bin", Flags::FlagMustCreate | Flags::ProtocolDirectory),
              Status::NotSupported);
    EXPECT_EQ(refused(writable, "config/readme", Flags::FlagCreateAsUnnamedTemporary),
              Status::NotSupported);
}

TEST_F(IoserviceServer, OpenRefusesWhatTheDesignCut) {
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "bin", Flags::ProtocolNode), Status::NotSupported);
    EXPECT_EQ(refused(root, "bin", Flags::FlagSendRepresentation), Status::NotSupported);
    EXPECT_EQ(refused(root, "nope", Flags::FlagSendRepresentation), Status::NotSupported);
}

TEST_F(IoserviceServer, OpenLookupFailures) {
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    EXPECT_EQ(refused(root, "nope", Flags::None), Status::NotFound);
    EXPECT_EQ(refused(root, "bin/nope", Flags::None), Status::NotFound);
    EXPECT_EQ(refused(root, "config/readme/more", Flags::None), Status::NotDir);
}

TEST_F(IoserviceServer, OpenMatchesTheProtocolAskedFor) {
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
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
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
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
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    // PERM_READABLE also asks for CONNECT, ENUMERATE and TRAVERSE, which a
    // file has no use for: they are dropped, not refused (design D21).
    auto const readme = opened(root, "config/readme", prot::io::PERM_READABLE);
    EXPECT_EQ(held(m_server.rights(readme)), Operations::ReadBytes | Operations::GetAttributes);
    // A directory keeps all it asked for: they bound what is opened below.
    auto const bin = opened(root, "bin", prot::io::PERM_READABLE);
    EXPECT_EQ(held(m_server.rights(bin)), prot::io::R_STAR_DIR);
    // The inherit flags are cut and ignored: the connection gets exactly the
    // rights the request names.
    auto const config = opened(root, "config", prot::io::PERM_READABLE | Flags::PermInheritExecute);
    EXPECT_EQ(held(m_server.rights(config)), prot::io::R_STAR_DIR);
}

TEST_F(IoserviceServer, ExecuteNeedsAnExecutableFile) {
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    auto const rx = Flags::PermReadBytes | Flags::PermExecute;
    EXPECT_EQ(refused(root, "config/readme", rx), Status::AccessDenied);
    auto const x = opened(root, "bin/x", rx);
    EXPECT_EQ(held(m_server.rights(x)), Operations::ReadBytes | Operations::Execute);

    // Nor more than the connection holds: a directory opened without
    // EXECUTE is a ceiling for the files under it.
    auto const bin = opened(root, "bin", prot::io::PERM_READABLE);
    EXPECT_EQ(refused(bin, "x", rx), Status::AccessDenied);
    auto const readable = bind(m_root, prot::io::R_STAR_DIR);
    EXPECT_EQ(refused(readable, "bin/x", rx), Status::AccessDenied);
}

TEST_F(IoserviceServer, WritingIsRefusedOnFiles) {
    auto const root = bind(m_root, prot::io::RW_STAR_DIR);
    EXPECT_EQ(refused(root, "config/readme", Flags::PermWriteBytes), Status::AccessDenied);
}

// --- ReadDirents ---------------------------------------------------------------

TEST_F(IoserviceServer, ReadDirentsListsDotFirstThenNameOrder) {
    using prot::io::DirentType;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const listing = read_dirents(root, prot::io::MAX_BUF);
    EXPECT_EQ(listing.status, Status::Ok);
    EXPECT_EQ(listing.names, (std::vector<std::string>{".", "bin", "config"}));
    EXPECT_EQ(listing.types, (std::vector<DirentType>{DirentType::Directory, DirentType::Directory,
                                                      DirentType::Directory}));
    // The end is an empty Ok.
    auto const end = read_dirents(root, prot::io::MAX_BUF);
    EXPECT_EQ(end.status, Status::Ok);
    EXPECT_TRUE(end.names.empty());

    auto const bin = opened(root, "bin", prot::io::PERM_READABLE);
    EXPECT_EQ(read_dirents(bin, prot::io::MAX_BUF).types,
              (std::vector<DirentType>{DirentType::Directory, DirentType::File}));
    auto const svc = bind(m_svc, prot::io::R_STAR_DIR);
    auto const services = read_dirents(svc, prot::io::MAX_BUF);
    EXPECT_EQ(services.names, (std::vector<std::string>{".", "echo"}));
    EXPECT_EQ(services.types,
              (std::vector<DirentType>{DirentType::Directory, DirentType::Service}));
}

TEST_F(IoserviceServer, ReadDirentsBufferTooSmall) {
    // An entry is 10 bytes and its name: "." 11, "bin" 13, "config" 16.
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
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
    EXPECT_EQ(read_dirents(root, prot::io::MAX_BUF).status, Status::AccessDenied);
}

TEST_F(IoserviceServer, ReadDirentsAboveTheLimitIsOutOfRange) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    EXPECT_EQ(read_dirents(root, prot::io::MAX_BUF + 1).status, Status::OutOfRange);
    EXPECT_EQ(read_dirents(root, ~u64{0}).status, Status::OutOfRange);
    // Nothing moved.
    EXPECT_EQ(read_dirents(root, prot::io::MAX_BUF).names.front(), ".");
}

// --- Read ----------------------------------------------------------------------

TEST_F(IoserviceServer, ReadMovesTheSeekToTheEnd) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = opened(root, "config/readme", prot::io::PERM_READABLE);
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
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = opened(root, "config/readme", prot::io::PERM_READABLE);
    EXPECT_EQ(read(readme, prot::io::MAX_TRANSFER_SIZE + 1).status, Status::OutOfRange);
    EXPECT_EQ(text_of(read(readme, prot::io::MAX_TRANSFER_SIZE).data), README);
}

TEST_F(IoserviceServer, ReadNeedsReadBytes) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = opened(root, "config/readme", Flags::PermGetAttributes);
    EXPECT_EQ(read(readme, 5).status, Status::BadHandle);
}

// --- GetBackingMemory --------------------------------------------------------

TEST_F(IoserviceServer, BackingMemoryRightsFollowTheFlags) {
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
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
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
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
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    auto const readme = opened(root, "config/readme", prot::io::PERM_READABLE);
    EXPECT_EQ(get_backing_memory(readme, VmoFlags::PrivateClone | VmoFlags::SharedBuffer).status,
              Status::InvArgs);
    EXPECT_EQ(get_backing_memory(readme, VmoFlags::Read | VmoFlags::Write).status,
              Status::AccessDenied);
    auto const attributes = opened(root, "config/readme", Flags::PermGetAttributes);
    EXPECT_EQ(get_backing_memory(attributes, VmoFlags::Read).status, Status::AccessDenied);
    EXPECT_EQ(duplicates().calls, 0u);
}

TEST_F(IoserviceServer, BackingMemoryPassesTheDuplicateFailureOn) {
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    auto const readme = opened(root, "config/readme", prot::io::PERM_READABLE);
    duplicates().answer = Status::NoResource;
    auto const reply = get_backing_memory(readme, VmoFlags::Read);
    EXPECT_EQ(reply.status, Status::NoResource);
    EXPECT_EQ(reply.vmo, Uandle::Invalid);
}

// --- Clone and Close -----------------------------------------------------------

TEST_F(IoserviceServer, CloneKeepsTheRightsAndStartsOver) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = opened(root, "config/readme", prot::io::PERM_READABLE);
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
    EXPECT_EQ(read_dirents(root, prot::io::MAX_BUF).names.size(), 3u);
    m_server.release(copy);
    auto const root_copy = clone(root, OTHER_OBJECT);
    ASSERT_EQ(root_copy.action, Action::Bind);
    EXPECT_EQ(held(m_server.rights(root_copy.connection)), prot::io::R_STAR_DIR);
    EXPECT_EQ(read_dirents(root_copy.connection, prot::io::MAX_BUF).names.front(), ".");
}

TEST_F(IoserviceServer, CloseAnswersThenReleases) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const outcome = dispatch(root, prot::io::encode_close(m_bytes, {.txid = TXID}));
    EXPECT_EQ(outcome.action, Action::ReplyAndClose);
    auto const reply = prot::io::decode_close_reply(outcome.message, TXID);
    ASSERT_TRUE(reply.is_ok());
    EXPECT_EQ(reply->status, Status::Ok);
    // The id is stale now, and anything sent on it is dropped.
    EXPECT_TRUE(m_server.channel(root).is_none());
    EXPECT_EQ(read_dirents_outcome(root), Action::Drop);
}

TEST_F(IoserviceServer, AFullTableRefusesWithNoResource) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    for (auto count = 1; count < 4; ++count) {
        bind(m_root, prot::io::R_STAR_DIR);
    }
    EXPECT_EQ(m_server.bind(CHANNEL, m_root, prot::io::R_STAR_DIR).unwrap_err(),
              Status::NoResource);
    EXPECT_EQ(refused(root, "bin", prot::io::PERM_READABLE), Status::NoResource);
    auto const cloned = clone(root, OBJECT);
    EXPECT_EQ(cloned.action, Action::Refuse);
    EXPECT_EQ(epitaph(cloned), Status::NoResource);

    // Releasing one frees its slot under a new generation: the old id stays
    // stale even though its slot serves again.
    m_server.release(root);
    auto const again = bind(m_root, prot::io::R_STAR_DIR);
    EXPECT_EQ(again.index, root.index);
    EXPECT_NE(again.generation, root.generation);
    EXPECT_TRUE(m_server.channel(root).is_none());
}

TEST_F(IoserviceServer, ARefusedOpenSaysWhatItAskedForAndWhy) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);

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
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    for (auto count = 1; count < 4; ++count) {
        bind(m_root, prot::io::R_STAR_DIR);
    }
    auto const cloned = clone(root, OBJECT);
    ASSERT_EQ(cloned.action, Action::Refuse);
    EXPECT_EQ(cloned.status, Status::NoResource);
    EXPECT_EQ(epitaph(cloned), cloned.status);
    EXPECT_TRUE(cloned.path.is_none());
}

// --- Services ------------------------------------------------------------------

TEST_F(IoserviceServer, OpeningAServiceHandsTheChannelBack) {
    auto const svc = bind(m_svc, prot::io::R_STAR_DIR);
    for (auto const flags: {Flags::ProtocolService, Flags::None}) {
        auto const outcome = open(svc, "echo", flags);
        EXPECT_EQ(outcome.action, Action::Connect);
        EXPECT_EQ(outcome.handle, OBJECT);
        EXPECT_EQ(outcome.service, ECHO);
    }
}

TEST_F(IoserviceServer, AServiceTakesNoOtherFlags) {
    auto const svc = bind(m_svc, prot::io::R_STAR_DIR);
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
        bytes_of_message(prot::io::encode_read(m_bytes, {.txid = TXID, .count = 1}));
    auto reply_bytes = read_bytes;
    reply_bytes[4] = u8{1}; // the direction: ToClient
    auto unknown_bytes = read_bytes;
    wire::store(std::span{unknown_bytes}, 8, u64{0x1234'5678});
    auto const open_bytes = bytes_of_message(prot::io::encode_open(
        m_bytes, m_handles, {.flags = Flags::None, .path = "x", .object = OBJECT}));
    auto const dirents_bytes =
        bytes_of_message(prot::io::encode_read_dirents(m_bytes, {.txid = TXID, .max_bytes = 100}));
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
        auto const root = bind(m_root, prot::io::RX_STAR_DIR | Operations::Execute);
        auto connection = root;
        if (test.node != m_root) {
            connection = opened(root, "config/readme", prot::io::PERM_READABLE);
        }
        auto const outcome = m_server.dispatch(
            connection, wire::Message{.bytes = test.bytes, .handles = test.handles}, m_out);
        EXPECT_EQ(outcome.action, Action::Drop) << test.what;
        EXPECT_TRUE(m_server.channel(connection).is_none()) << test.what;
        m_server.release(root);
    }
}

TEST_F(IoserviceServer, AStaleIdIsDropped) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    m_server.release(root);
    EXPECT_EQ(read_dirents_outcome(root), Action::Drop);
    EXPECT_EQ(read_dirents_outcome(ConnectionId{.index = 99, .generation = 0}), Action::Drop);
    // Releasing it again changes nothing.
    m_server.release(root);
    auto const next = bind(m_root, prot::io::R_STAR_DIR);
    EXPECT_EQ(next.generation, root.generation + 1);
}

TEST_F(IoserviceServer, GenerationZeroNamesNoConnection) {
    // A slot's first connection has generation 1, so an id nobody filled in,
    // {0, 0}, does not name the connection in slot 0 even while it is live.
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    EXPECT_EQ(root.index, 0U);
    EXPECT_EQ(root.generation, ioservice::FIRST_GENERATION);
    EXPECT_TRUE(m_server.channel(ConnectionId{}).is_none());
    EXPECT_EQ(read_dirents_outcome(ConnectionId{}), Action::Drop);
    // The Drop for the stale id left the live connection alone.
    EXPECT_TRUE(m_server.channel(root).is_some());
}

// --- bind ------------------------------------------------------------------------

TEST_F(IoserviceServer, BindRefusals) {
    EXPECT_EQ(m_server.bind(Uandle::Invalid, m_root, prot::io::R_STAR_DIR).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(m_server.bind(CHANNEL, NodeId{15}, prot::io::R_STAR_DIR).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(m_server.bind(CHANNEL, m_root, static_cast<Operations>(u64{0x200})).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(m_server.bind(CHANNEL, m_readme, prot::io::R_STAR_DIR).unwrap_err(), Status::NotDir);
    EXPECT_EQ(m_server.bind(CHANNEL, m_echo, prot::io::R_STAR_DIR).unwrap_err(), Status::NotDir);
    // A remote entry is not one of this server's directories either: what it
    // stands for is another server's to bind.
    EXPECT_EQ(m_server.bind(CHANNEL, m_boot, prot::io::R_STAR_DIR).unwrap_err(), Status::NotDir);
}

// --- Open: forwarding through a remote entry --------------------------------------

TEST_F(IoserviceServer, OpenThroughARemoteEntryForwardsTheRestOfThePath) {
    auto const mount = bind(m_mount, prot::io::RX_STAR_DIR);
    auto const outcome =
        open(mount, "mnt/boot/config/readme", Flags::PermReadBytes | Flags::PermGetAttributes);
    EXPECT_EQ(outcome.action, Action::Forward);
    EXPECT_EQ(outcome.forward_channel, REMOTE);
    auto const forwarded = prot::io::decode_open(outcome.message);
    ASSERT_TRUE(forwarded.is_ok());
    EXPECT_EQ(forwarded->path, "config/readme");
    EXPECT_EQ(forwarded->flags, Flags::PermReadBytes | Flags::PermGetAttributes);
    EXPECT_EQ(forwarded->object, OBJECT);
}

TEST_F(IoserviceServer, OpenOfTheMountPointItselfForwardsDot) {
    auto const mount = bind(m_mount, prot::io::RX_STAR_DIR);
    auto const outcome = open(mount, "mnt/boot", Flags::PermReadBytes);
    EXPECT_EQ(outcome.action, Action::Forward);
    EXPECT_EQ(outcome.forward_channel, REMOTE);
    auto const forwarded = prot::io::decode_open(outcome.message);
    ASSERT_TRUE(forwarded.is_ok());
    EXPECT_EQ(forwarded->path, ".");
    EXPECT_EQ(forwarded->object, OBJECT);
}

TEST_F(IoserviceServer, OpenThroughARemoteEntryCarriesCreationFlags) {
    // A local target refuses to create (OpenRefusesToCreate); a remote one
    // is for the server that owns the subtree to judge, so the flags pass
    // through unchanged.
    auto const mount = bind(m_mount, prot::io::RW_STAR_DIR);
    auto const flags = prot::io::PERM_WRITABLE | Flags::FlagMaybeCreate;
    auto const outcome = open(mount, "mnt/boot/new", flags);
    EXPECT_EQ(outcome.action, Action::Forward);
    auto const forwarded = prot::io::decode_open(outcome.message);
    ASSERT_TRUE(forwarded.is_ok());
    EXPECT_EQ(forwarded->flags, flags);
}

TEST_F(IoserviceServer, OpenThroughARemoteEntryStillChecksRightsFirst) {
    // Check 3 runs before anything is forwarded: a mount point is no way
    // around a connection's ceiling.
    auto const mount = bind(m_mount, prot::io::R_STAR_DIR);
    EXPECT_EQ(refused(mount, "mnt/boot/new", Flags::PermWriteBytes), Status::AccessDenied);
}

TEST_F(IoserviceServer, LocalCreationIsStillRefusedBesideTheMount) {
    auto const mount = bind(m_mount, prot::io::RW_STAR_DIR);
    EXPECT_EQ(refused(mount, "mnt/new", prot::io::PERM_WRITABLE | Flags::FlagMaybeCreate),
              Status::NotSupported);
}

TEST_F(IoserviceServer, AMountPointListsAsADirectory) {
    auto const mount = bind(m_mount, prot::io::R_STAR_DIR);
    auto const mnt = opened(mount, "mnt", Flags::PermEnumerate);
    auto const listing = read_dirents(mnt, prot::io::MAX_BUF);
    EXPECT_EQ(listing.status, Status::Ok);
    // "." first, then the mount point standing in for the directory the
    // remote serves.
    ASSERT_EQ(listing.names.size(), 2U);
    EXPECT_EQ(listing.names[0], ".");
    EXPECT_EQ(listing.names[1], "boot");
    EXPECT_EQ(listing.types[1], prot::io::DirentType::Directory);
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
    auto const root = bind(m_root, prot::io::R_STAR_DIR);

    // The value, the rest of the path after the node, and the channel; and
    // the node itself is where the walk ended, so "." is its rest.
    EXPECT_EQ(handed_over(root, "mnt/fs", prot::io::PERM_READABLE).rest, ".");
    EXPECT_EQ(handed_over(root, "mnt/fs/a", prot::io::PERM_READABLE).rest, "a");
    EXPECT_EQ(handed_over(root, "mnt/fs/a/b/c", prot::io::PERM_READABLE).rest, "a/b/c");

    // From a connection further down, the path starts there.
    auto const mnt = opened(root, "mnt", Flags::ProtocolDirectory | prot::io::PERM_READABLE);
    EXPECT_EQ(handed_over(mnt, "fs/x/y", prot::io::PERM_READABLE).rest, "x/y");
    EXPECT_EQ(handed_over(mnt, "fs", prot::io::PERM_READABLE).rest, ".");
}

TEST_F(IoserviceServerSubtree, TheProgramGetsTheFlagsAsTheClientSentThem) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    // Whatever the client asked for, within what the connection holds, is
    // passed on whole: the protocol bits the node would not judge, the file
    // flags that a directory refuses, a bit nobody defines. The flags are the
    // program's tree's to judge, for what it is.
    for (auto const flags: {
             Flags::None,
             prot::io::PERM_READABLE,
             Flags::ProtocolDirectory | prot::io::PERM_READABLE,
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
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    for (auto count = 0; count < 12; ++count) {
        EXPECT_EQ(handed_over(root, "mnt/fs/x", prot::io::PERM_READABLE).action, Action::Subtree);
    }
    bind(m_root, prot::io::R_STAR_DIR);
    bind(m_root, prot::io::R_STAR_DIR);
    bind(m_root, prot::io::R_STAR_DIR);
    // The table is now full of connections that were bound, and an Open of a
    // directory finds it so; the Subtree Open still reaches the program,
    // since it asks for no slot.
    EXPECT_EQ(refused(root, "mnt", prot::io::PERM_READABLE), Status::NoResource);
    EXPECT_EQ(handed_over(root, "mnt/fs/x", prot::io::PERM_READABLE).action, Action::Subtree);
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
    constexpr auto SERVED = prot::io::PERM_READABLE;
    for (auto const &c: {
             // 1. The path's syntax, whatever the node would have been.
             Case{"a leading slash", prot::io::R_STAR_DIR, "/mnt/fs/x", SERVED, Status::BadPath},
             Case{"a dot-dot after the node", prot::io::R_STAR_DIR, "mnt/fs/../x", SERVED,
                  Status::BadPath},
             Case{"an empty segment after the node", prot::io::R_STAR_DIR, "mnt/fs//x", SERVED,
                  Status::BadPath},
             Case{"a trailing slash", prot::io::R_STAR_DIR, "mnt/fs/", SERVED, Status::BadPath},
             Case{"an empty path", prot::io::R_STAR_DIR, "", SERVED, Status::BadPath},
             // 2. Going below the directory needs TRAVERSE.
             Case{"no traverse",
                  Operations::Connect | Operations::Enumerate | Operations::ReadBytes, "mnt/fs/x",
                  Flags::PermReadBytes, Status::AccessDenied},
             // 3. Rights are a subset of the connection's.
             Case{"write on a reader", prot::io::R_STAR_DIR, "mnt/fs/x",
                  Flags::PermReadBytes | Flags::PermWriteBytes, Status::AccessDenied},
             Case{"modify on a reader", prot::io::R_STAR_DIR, "mnt/fs/x",
                  Flags::PermModifyDirectory, Status::AccessDenied},
             Case{"execute on a reader", prot::io::R_STAR_DIR, "mnt/fs", Flags::PermExecute,
                  Status::AccessDenied},
             // 4. Connecting needs CONNECT.
             Case{"service without connect",
                  Operations::Enumerate | Operations::Traverse | Operations::ReadBytes, "mnt/fs/x",
                  Flags::ProtocolService, Status::AccessDenied},
             // 6. Creating, and what the design cut.
             Case{"create without modify", prot::io::R_STAR_DIR, "mnt/fs/new",
                  Flags::FlagMaybeCreate | Flags::ProtocolFile, Status::AccessDenied},
             Case{"create with modify", prot::io::R_STAR_DIR | Operations::ModifyDirectory,
                  "mnt/fs/new", Flags::FlagMustCreate | Flags::ProtocolFile | Flags::PermReadBytes,
                  Status::NotSupported},
             Case{"a node-only connection", prot::io::R_STAR_DIR, "mnt/fs/x", Flags::ProtocolNode,
                  Status::NotSupported},
             Case{"a representation event", prot::io::R_STAR_DIR, "mnt/fs",
                  Flags::FlagSendRepresentation, Status::NotSupported},
             // 8. The walk to the node.
             Case{"a name missing before the node", prot::io::R_STAR_DIR, "nope/fs/x", SERVED,
                  Status::NotFound},
             Case{"a name missing next to the node", prot::io::R_STAR_DIR, "mnt/nope/x", SERVED,
                  Status::NotFound},
             Case{"a file before the node", prot::io::R_STAR_DIR, "mnt/note/fs/x", SERVED,
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
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const outcome = open(root, "mnt/fs/x", Flags::PermWriteBytes);
    ASSERT_EQ(outcome.action, Action::Refuse);
    EXPECT_EQ(outcome.status, Status::AccessDenied);
    ASSERT_TRUE(outcome.path.is_some());
    EXPECT_EQ(*outcome.path, "mnt/fs/x");
}

TEST_F(IoserviceServerSubtree, ASubtreeNodeIsListedAsADirectory) {
    auto const mnt = bind(m_mnt, prot::io::R_STAR_DIR);
    auto const listing = read_dirents(mnt, prot::io::MAX_BUF);
    EXPECT_EQ(listing.status, Status::Ok);
    EXPECT_EQ(listing.names, (std::vector<std::string>{".", "fs", "note"}));
    EXPECT_EQ(listing.types, (std::vector<prot::io::DirentType>{prot::io::DirentType::Directory,
                                                                prot::io::DirentType::Directory,
                                                                prot::io::DirentType::File}));
}

TEST_F(IoserviceServerSubtree, NoConnectionIsBoundToASubtreeNode) {
    EXPECT_EQ(m_server.bind(CHANNEL, m_fs, prot::io::R_STAR_DIR).unwrap_err(), Status::NotDir);
}

TEST_F(IoserviceServerSubtree, ThePathOfAnOpenThatMissesTheNodeIsNotHandedOver) {
    // A sibling of the node, and a file next to it, are this tree's own.
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const note = open(root, "mnt/note", Flags::ProtocolFile | Flags::PermReadBytes);
    EXPECT_EQ(note.action, Action::Bind);
    auto const mnt = open(root, "mnt", Flags::ProtocolDirectory | prot::io::PERM_READABLE);
    EXPECT_EQ(mnt.action, Action::Bind);
}

// --- Handler nodes -------------------------------------------------------------------

/// The tree of IoserviceServer, with places a NodeHandler answers, and a twin
/// of the handler's content in the tree, to compare replies with:
///
///   root/fs               a Handler node, FS
///   root/other            a Handler node, OTHER_FS
///   twin/                 a directory: bin/x (executable) and config/readme
class IoserviceServerHandlerNode: public IoserviceServer {
  protected:
    /// What the nodes were added with.
    static constexpr auto FS = u64{77};
    static constexpr auto OTHER_FS = u64{78};
    static constexpr auto SENTINEL = Uandle{0x7777};

    /// Room for the connections the handler tests hold at once.
    static constexpr auto CONNECTIONS = usize{16};

    IoserviceServerHandlerNode() : IoserviceServer(CONNECTIONS) {
        m_fs_node = m_tree.add_handler(m_root, "fs", FS).unwrap();
        m_other_node = m_tree.add_handler(m_root, "other", OTHER_FS).unwrap();
        m_twin = m_tree.add_root().unwrap();
        auto const bin = m_tree.add_directory(m_twin, "bin").unwrap();
        m_twin_x = m_tree
                       .add_file(bin, "x",
                                 {.vmo = VMO_X, .bytes = bytes_of(X_CONTENTS), .executable = true})
                       .unwrap();
        auto const config = m_tree.add_directory(m_twin, "config").unwrap();
        m_twin_readme =
            m_tree
                .add_file(config, "readme",
                          {.vmo = VMO_README, .bytes = bytes_of(README), .executable = false})
                .unwrap();
        m_fs.server = &m_server;
        m_fs.out = &m_out;
    }

    /// A reply as it was written, copied out of the buffer that the next call
    /// builds over.
    struct Raw {
        Action action;
        std::vector<u8> bytes;
        std::vector<Uandle> handles;

        friend auto operator==(Raw const &, Raw const &) -> bool = default;
    };

    auto raw(ConnectionId const connection, ustl::Result<wire::Message, Status> const &message)
        -> Raw {
        auto const outcome = dispatch(connection, message);
        return {
            outcome.action,
            std::vector<u8>(outcome.message.bytes.begin(), outcome.message.bytes.end()),
            std::vector<Uandle>(outcome.message.handles.begin(), outcome.message.handles.end())};
    }

    auto raw_read(ConnectionId const connection, u64 const count) -> Raw {
        return raw(connection, prot::io::encode_read(m_bytes, {.txid = TXID, .count = count}));
    }

    auto raw_listing(ConnectionId const connection, u64 const max_bytes) -> Raw {
        return raw(connection,
                   prot::io::encode_read_dirents(m_bytes, {.txid = TXID, .max_bytes = max_bytes}));
    }

    auto raw_vmo(ConnectionId const connection, VmoFlags const flags) -> Raw {
        return raw(connection,
                   prot::io::encode_get_backing_memory(m_bytes, {.txid = TXID, .flags = flags}));
    }

    /// The directory connection to the handler's own root, with |rights|.
    auto handler_root(ConnectionId const parent, Flags const rights = prot::io::PERM_READABLE)
        -> ConnectionId {
        return opened(parent, "fs", Flags::ProtocolDirectory | rights);
    }

    /// Takes every connection that is free, so that the next one is refused.
    auto fill_table() -> void {
        while (m_server.bind(CHANNEL, m_root, prot::io::R_STAR_DIR).is_ok()) {
        }
    }

    NodeId m_fs_node{};
    NodeId m_other_node{};
    NodeId m_twin{};
    NodeId m_twin_x{};
    NodeId m_twin_readme{};
};

/// Each of these runs twice: with a handler that answers every operation at
/// once, and with one that answers Pending and ends the request through
/// complete() before the test looks at the reply. Both must read the same.
class IoserviceServerHandler: public IoserviceServerHandlerNode,
                              public testing::WithParamInterface<bool> {
  protected:
    IoserviceServerHandler() {
        if (GetParam()) {
            m_fs.pend = PEND_ALL;
        }
    }

    auto settle(Outcome const outcome) -> Outcome override {
        if (outcome.action != Action::Pending) {
            return outcome;
        }
        EXPECT_FALSE(m_fs.parked.empty());
        if (m_fs.parked.empty()) {
            return Outcome{.action = Action::Drop};
        }
        auto const completed = m_fs.complete(m_server, m_out, m_fs.parked.back().ticket);
        EXPECT_TRUE(completed.is_ok());
        if (completed.is_err()) {
            return Outcome{.action = Action::Drop};
        }
        return completed.unwrap();
    }
};

INSTANTIATE_TEST_SUITE_P(Answers, IoserviceServerHandler, testing::Values(false, true),
                         [](testing::TestParamInfo<bool> const &info) {
                             if (info.param) {
                                 return "Pending";
                             }
                             return "Done";
                         });

TEST_P(IoserviceServerHandler, AnOpenThatWalksToAHandlerNodeIsAskedOfTheHandler) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);

    // The node itself: the handler's own root, "." of it.
    auto const flags = Flags::ProtocolDirectory | prot::io::PERM_READABLE;
    auto const fs = opened(root, "fs", flags);
    ASSERT_EQ(m_fs.opens.size(), 1u);
    EXPECT_EQ(m_fs.opens[0].value, FS);
    EXPECT_TRUE(m_fs.opens[0].directory.is_none());
    EXPECT_EQ(m_fs.opens[0].path, ".");
    EXPECT_EQ(m_fs.opens[0].flags, flags);
    EXPECT_EQ(m_fs.opens[0].ticket.connection, root);
    // The connection is to the handler's place, with exactly the rights asked.
    EXPECT_EQ(held(m_server.rights(fs)), prot::io::R_STAR_DIR);
    EXPECT_EQ(held(m_server.node(fs)), m_fs_node);

    // The rest of the path after the node, and a file's rights shrink to the
    // ones the file protocol uses.
    auto const readme = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    ASSERT_EQ(m_fs.opens.size(), 2u);
    EXPECT_EQ(m_fs.opens[1].value, FS);
    EXPECT_TRUE(m_fs.opens[1].directory.is_none());
    EXPECT_EQ(m_fs.opens[1].path, "config/readme");
    EXPECT_EQ(held(m_server.rights(readme)), Operations::ReadBytes | Operations::GetAttributes);

    // Another node is another value.
    opened(root, "other/bin", prot::io::PERM_READABLE);
    ASSERT_EQ(m_fs.opens.size(), 3u);
    EXPECT_EQ(m_fs.opens[2].value, OTHER_FS);
    EXPECT_EQ(m_fs.opens[2].path, "bin");
}

TEST_P(IoserviceServerHandler, AnOpenThroughAHandlersDirectoryAsksTheHandlerWithItsCookie) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const fs = handler_root(root);
    auto const config = opened(fs, "config", Flags::ProtocolDirectory | prot::io::PERM_READABLE);
    // The Open came on the directory's connection, so the handler is told
    // which directory, and the path is the whole path the client sent.
    ASSERT_EQ(m_fs.opens.size(), 2u);
    EXPECT_EQ(m_fs.opens[1].value, FS);
    EXPECT_EQ(held(m_fs.opens[1].directory), m_fs.cookies[0]);
    EXPECT_EQ(m_fs.opens[1].path, "config");

    auto const readme = opened(config, "readme", prot::io::PERM_READABLE);
    ASSERT_EQ(m_fs.opens.size(), 3u);
    EXPECT_EQ(held(m_fs.opens[2].directory), m_fs.cookies[1]);
    EXPECT_EQ(m_fs.opens[2].path, "readme");
    EXPECT_EQ(text_of(read(readme, 100).data), README);

    // Rights only shrink down the chain, and "." names the directory again.
    EXPECT_EQ(refused(config, "readme", prot::io::PERM_READABLE | Flags::PermWriteBytes),
              Status::AccessDenied);
    opened(config, ".", Flags::ProtocolDirectory | Flags::PermEnumerate);
    EXPECT_EQ(m_fs.opens.back().path, ".");
    EXPECT_EQ(refused(config, "nope", Flags::None), Status::NotFound);
}

TEST_P(IoserviceServerHandler, TheConnectionsOwnChecksRunBeforeTheHandlerIsAsked) {
    struct Case {
        char const *what;
        Operations held;
        char const *path;
        Flags flags;
        Status expected;
    };
    constexpr auto SERVED = prot::io::PERM_READABLE;
    for (auto const &c: {
             // 1. The path's syntax.
             Case{"a leading slash", prot::io::R_STAR_DIR, "/fs/x", SERVED, Status::BadPath},
             Case{"a dot-dot after the node", prot::io::R_STAR_DIR, "fs/../x", SERVED,
                  Status::BadPath},
             Case{"an empty segment after the node", prot::io::R_STAR_DIR, "fs//x", SERVED,
                  Status::BadPath},
             Case{"a trailing slash", prot::io::R_STAR_DIR, "fs/x/", SERVED, Status::BadPath},
             // 2. Going below the directory needs TRAVERSE.
             Case{"no traverse",
                  Operations::Connect | Operations::Enumerate | Operations::ReadBytes, "fs/x",
                  Flags::PermReadBytes, Status::AccessDenied},
             // 3. Rights are a subset of the connection's.
             Case{"write on a reader", prot::io::R_STAR_DIR, "fs/x",
                  Flags::PermReadBytes | Flags::PermWriteBytes, Status::AccessDenied},
             Case{"modify on a reader", prot::io::R_STAR_DIR, "fs/x", Flags::PermModifyDirectory,
                  Status::AccessDenied},
             Case{"execute on a reader", prot::io::R_STAR_DIR, "fs", Flags::PermExecute,
                  Status::AccessDenied},
             // 4. Connecting needs CONNECT.
             Case{"service without connect",
                  Operations::Enumerate | Operations::Traverse | Operations::ReadBytes, "fs/x",
                  Flags::ProtocolService, Status::AccessDenied},
             // 6 and 7. Creating, and what the design cut.
             Case{"create without modify", prot::io::R_STAR_DIR, "fs/new",
                  Flags::FlagMaybeCreate | Flags::ProtocolFile, Status::AccessDenied},
             Case{"create with modify", prot::io::R_STAR_DIR | Operations::ModifyDirectory,
                  "fs/new", Flags::FlagMustCreate | Flags::ProtocolFile | Flags::PermReadBytes,
                  Status::NotSupported},
             Case{"a node-only connection", prot::io::R_STAR_DIR, "fs/x", Flags::ProtocolNode,
                  Status::NotSupported},
             Case{"a representation event", prot::io::R_STAR_DIR, "fs",
                  Flags::FlagSendRepresentation, Status::NotSupported},
             // 8. The walk to the node.
             Case{"a name missing before the node", prot::io::R_STAR_DIR, "nope/fs/x", SERVED,
                  Status::NotFound},
             Case{"a file before the node", prot::io::R_STAR_DIR, "config/readme/fs/x", SERVED,
                  Status::NotDir},
         }) {
        auto const connection = bind(m_root, c.held);
        EXPECT_EQ(refused(connection, c.path, c.flags), c.expected) << c.what;
        m_server.release(connection);
    }
    EXPECT_TRUE(m_fs.opens.empty());
}

TEST_P(IoserviceServerHandler, TheChecksRunOnADirectoryTheHandlerOpenedBeforeItIsAskedAgain) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    // A reader that may list but not walk, and one that may walk and read.
    auto const lister = handler_root(root, Flags::PermEnumerate);
    auto const walker = handler_root(root, prot::io::PERM_READABLE);
    ASSERT_EQ(m_fs.opens.size(), 2u);

    EXPECT_EQ(refused(lister, "config", Flags::None), Status::AccessDenied);
    EXPECT_EQ(refused(walker, "/config", Flags::None), Status::BadPath);
    EXPECT_EQ(refused(walker, "config/../bin", Flags::None), Status::BadPath);
    EXPECT_EQ(refused(walker, "config", Flags::PermWriteBytes), Status::AccessDenied);
    EXPECT_EQ(refused(walker, "config", Flags::PermExecute), Status::AccessDenied);
    EXPECT_EQ(refused(walker, "config", Flags::FlagMaybeCreate), Status::AccessDenied);
    EXPECT_EQ(refused(walker, "config", Flags::ProtocolNode), Status::NotSupported);
    EXPECT_EQ(refused(walker, "config", Flags::FlagSendRepresentation), Status::NotSupported);
    // Nothing above reached the handler.
    EXPECT_EQ(m_fs.opens.size(), 2u);
    // And an Open the checks pass does.
    opened(walker, "config", prot::io::PERM_READABLE);
    EXPECT_EQ(m_fs.opens.size(), 3u);
}

TEST_P(IoserviceServerHandler, TheKindTheHandlerReportsMeetsTheFlags) {
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    auto const rw = bind(m_root, prot::io::RW_STAR_DIR);

    // A directory for a request that accepts only files, and the reverse.
    EXPECT_EQ(refused(root, "fs/config", Flags::ProtocolFile), Status::NotFile);
    EXPECT_EQ(refused(root, "fs/config/readme", Flags::ProtocolDirectory), Status::NotDir);
    // A protocol that is neither.
    EXPECT_EQ(refused(root, "fs/config", Flags::ProtocolSymlink), Status::BadType);
    EXPECT_EQ(refused(root, "fs/config", Flags::ProtocolService), Status::BadType);
    EXPECT_EQ(refused(root, "fs/config/readme", Flags::ProtocolSymlink), Status::BadType);
    // Flags the kind does not take.
    EXPECT_EQ(refused(root, "fs/bin", Flags::FileAppend), Status::InvArgs);
    EXPECT_EQ(refused(root, "fs/bin", Flags::FileTruncate), Status::InvArgs);
    EXPECT_EQ(refused(root, "fs/config/readme", Flags::PermReadBytes | Flags::FileTruncate),
              Status::InvArgs);
    // A file the subset never writes.
    EXPECT_EQ(refused(rw, "fs/config/readme", Flags::PermWriteBytes), Status::AccessDenied);
    // Every object the handler made for a request the Server then refused was
    // released, once: none is live.
    EXPECT_EQ(m_fs.opens.size(), 9u);
    EXPECT_TRUE(m_fs.live.empty());
    EXPECT_EQ(m_fs.releases.size(), 9u);
    EXPECT_EQ(m_fs.bad_releases, 0u);

    // Several protocols accept the kind there is.
    auto const either = Flags::ProtocolDirectory | Flags::ProtocolFile;
    auto const bin = opened(root, "fs/bin", either);
    EXPECT_EQ(held(m_server.rights(bin)), Operations::None);
    auto const readme = opened(root, "fs/config/readme", either | Flags::PermReadBytes);
    EXPECT_EQ(held(m_server.rights(readme)), Operations::ReadBytes);
    opened(root, "fs/config/readme", Flags::PermReadBytes | Flags::FileAppend);

    // EXECUTE on a file is the handler's to judge, since the Server cannot
    // tell: it refused one file and let the other through.
    auto const rx = Flags::PermReadBytes | Flags::PermExecute;
    EXPECT_EQ(refused(root, "fs/config/readme", rx), Status::AccessDenied);
    auto const x = opened(root, "fs/bin/x", rx);
    EXPECT_EQ(held(m_server.rights(x)), Operations::ReadBytes | Operations::Execute);
}

TEST_P(IoserviceServerHandler, TheRepliesAreTheOnesATreeOfTheSameContentGives) {
    auto const root = bind(m_root, prot::io::RX_STAR_DIR);
    auto const twin = bind(m_twin, prot::io::RX_STAR_DIR);

    // A file: Read in pieces and to the end, a count above the limit, and a
    // connection that cannot read.
    auto const tree_file = opened(twin, "config/readme", prot::io::PERM_READABLE);
    auto const handled_file = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    for (auto const count: {u64{5}, u64{100}, u64{10}, u64{prot::io::MAX_TRANSFER_SIZE + 1}}) {
        auto const expected = raw_read(tree_file, count);
        EXPECT_EQ(expected.action, Action::Reply);
        EXPECT_EQ(raw_read(handled_file, count), expected) << count;
    }
    auto const tree_blind = opened(twin, "config/readme", Flags::PermGetAttributes);
    auto const handled_blind = opened(root, "fs/config/readme", Flags::PermGetAttributes);
    EXPECT_EQ(raw_read(handled_blind, 5), raw_read(tree_blind, 5));

    // GetBackingMemory with every flag a request can send, rights that are
    // not there included.
    auto const tree_x = opened(twin, "bin/x", Flags::PermReadBytes | Flags::PermExecute);
    auto const handled_x = opened(root, "fs/bin/x", Flags::PermReadBytes | Flags::PermExecute);
    for (auto const flags: {
             VmoFlags::Read,
             VmoFlags::Read | VmoFlags::PrivateClone,
             VmoFlags::Read | VmoFlags::Execute | VmoFlags::PrivateClone,
             VmoFlags::PrivateClone | VmoFlags::SharedBuffer,
             VmoFlags::Read | VmoFlags::Write,
         }) {
        auto const expected = raw_vmo(tree_x, flags);
        EXPECT_EQ(raw_vmo(handled_x, flags), expected) << std::to_underlying(flags);
    }
    EXPECT_EQ(raw_vmo(handled_blind, VmoFlags::Read), raw_vmo(tree_blind, VmoFlags::Read));

    // A directory: the listing at once, and the sequence of limits that cuts
    // it in every place, the "." included.
    for (auto const &limits: {
             std::vector<u64>{prot::io::MAX_BUF, prot::io::MAX_BUF},
             std::vector<u64>{10, 0, 11, 12, 28, 15, 16, 16},
             std::vector<u64>{prot::io::MAX_BUF + 1, 11, 13, 29, 16},
         }) {
        auto const tree_dir = opened(twin, ".", Flags::ProtocolDirectory | prot::io::PERM_READABLE);
        auto const handled_dir = handler_root(root);
        for (auto const limit: limits) {
            auto const expected = raw_listing(tree_dir, limit);
            EXPECT_EQ(raw_listing(handled_dir, limit), expected) << limit;
        }
    }
    // A directory that cannot enumerate.
    auto const tree_blind_dir = opened(twin, ".", Flags::ProtocolDirectory | Flags::PermTraverse);
    auto const handled_blind_dir = handler_root(root, Flags::PermTraverse);
    EXPECT_EQ(raw_listing(handled_blind_dir, 100), raw_listing(tree_blind_dir, 100));
}

TEST_P(IoserviceServerHandler, ARequestTheConnectionCannotMakeIsRefusedBeforeTheHandlerIsAsked) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const blind = opened(root, "fs/config/readme", Flags::PermGetAttributes);
    auto const readme = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    auto const walker = handler_root(root, Flags::PermTraverse);
    auto const lister = handler_root(root);

    // A Read without READ_BYTES, and one past the limit.
    EXPECT_EQ(read(blind, 5).status, Status::BadHandle);
    EXPECT_EQ(read(readme, prot::io::MAX_TRANSFER_SIZE + 1).status, Status::OutOfRange);
    EXPECT_TRUE(m_fs.reads.empty());
    // A ReadDirents without ENUMERATE, and one past the limit.
    EXPECT_EQ(read_dirents(walker, prot::io::MAX_BUF).status, Status::AccessDenied);
    EXPECT_EQ(read_dirents(lister, prot::io::MAX_BUF + 1).status, Status::OutOfRange);
    EXPECT_TRUE(m_fs.listings.empty());
    // A first call with no room for the "." entry.
    EXPECT_EQ(read_dirents(lister, 10).status, Status::BufferTooSmall);
    EXPECT_TRUE(m_fs.listings.empty());
    // A GetBackingMemory asking for rights the connection lacks, or two kinds
    // of copy at once.
    EXPECT_EQ(get_backing_memory(blind, VmoFlags::Read).status, Status::AccessDenied);
    EXPECT_EQ(get_backing_memory(readme, VmoFlags::Read | VmoFlags::Write).status,
              Status::AccessDenied);
    EXPECT_EQ(get_backing_memory(readme, VmoFlags::Read | VmoFlags::Execute).status,
              Status::AccessDenied);
    EXPECT_EQ(get_backing_memory(readme, VmoFlags::PrivateClone | VmoFlags::SharedBuffer).status,
              Status::InvArgs);
    EXPECT_TRUE(m_fs.vmos.empty());
}

TEST_P(IoserviceServerHandler, TheHandlerIsGivenTheConnectionsOffsetAndItMovesByTheBytesRead) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = opened(root, "fs/config/readme", prot::io::PERM_READABLE);

    EXPECT_EQ(text_of(read(readme, 5).data), "hello");
    EXPECT_EQ(text_of(read(readme, 3).data), ", w");
    // Fewer bytes than asked for: it moves by what came, not by the count.
    EXPECT_EQ(text_of(read(readme, 100).data), "orld");
    EXPECT_TRUE(read(readme, 10).data.empty());
    // A Read the handler fails moves nothing.
    m_fs.read_error = Status::NotReachable;
    EXPECT_EQ(read(readme, 4).status, Status::NotReachable);
    m_fs.read_error = Status::Ok;
    EXPECT_TRUE(read(readme, 4).data.empty());

    ASSERT_EQ(m_fs.reads.size(), 6u);
    auto offsets = std::vector<u64>{};
    auto counts = std::vector<u64>{};
    for (auto const &call: m_fs.reads) {
        offsets.push_back(call.offset);
        counts.push_back(call.count);
        EXPECT_EQ(call.value, FS);
        EXPECT_EQ(call.cookie, m_fs.cookies[0]);
    }
    EXPECT_EQ(offsets, (std::vector<u64>{0, 5, 8, 12, 12, 12}));
    EXPECT_EQ(counts, (std::vector<u64>{5, 3, 100, 10, 4, 4}));
}

TEST_P(IoserviceServerHandler, TheServerKeepsTheDotEntryAndTheCursorOfAListing) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const fs = handler_root(root);

    // The first call is given room for what follows the "." and the cursor 0.
    auto const first = read_dirents(fs, prot::io::MAX_BUF);
    EXPECT_EQ(first.names, (std::vector<std::string>{".", "bin", "config"}));
    EXPECT_EQ(first.types, (std::vector<prot::io::DirentType>{prot::io::DirentType::Directory,
                                                              prot::io::DirentType::Directory,
                                                              prot::io::DirentType::Directory}));
    // The next call is given the cursor the handler named, and the room in
    // full: the "." has been sent.
    auto const end = read_dirents(fs, prot::io::MAX_BUF);
    EXPECT_EQ(end.status, Status::Ok);
    EXPECT_TRUE(end.names.empty());

    ASSERT_EQ(m_fs.listings.size(), 2u);
    EXPECT_EQ(m_fs.listings[0].value, FS);
    EXPECT_EQ(m_fs.listings[0].cookie, m_fs.cookies[0]);
    EXPECT_EQ(m_fs.listings[0].cursor, 0u);
    EXPECT_EQ(m_fs.listings[0].max_bytes, prot::io::MAX_BUF - 11);
    EXPECT_EQ(m_fs.listings[1].cursor, 2u);
    EXPECT_EQ(m_fs.listings[1].max_bytes, prot::io::MAX_BUF);

    // A listing the handler fails leaves the connection where it was, "." and
    // all, and the same goes for a first entry that does not fit after it.
    auto const again = handler_root(root);
    EXPECT_EQ(read_dirents(again, 11).names, (std::vector<std::string>{"."}));
    EXPECT_EQ(read_dirents(again, 12).status, Status::BufferTooSmall);
    EXPECT_EQ(m_fs.listings.back().cursor, 0u);
    EXPECT_EQ(read_dirents(again, 28).names, (std::vector<std::string>{"bin"}));
    EXPECT_EQ(read_dirents(again, 16).names, (std::vector<std::string>{"config"}));
    EXPECT_EQ(m_fs.listings.back().cursor, 1u);
}

TEST_P(IoserviceServerHandler, ACloneAsksTheHandlerForANewCookieAndStartsOver) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    EXPECT_EQ(text_of(read(readme, 5).data), "hello");

    auto const outcome = clone(readme, OTHER_OBJECT);
    ASSERT_EQ(outcome.action, Action::Bind);
    EXPECT_EQ(outcome.handle, OTHER_OBJECT);
    auto const copy = outcome.connection;
    ASSERT_EQ(m_fs.clones.size(), 1u);
    EXPECT_EQ(m_fs.clones[0], (std::pair<u64, u64>{FS, m_fs.cookies[0]}));
    EXPECT_EQ(held(m_server.rights(copy)), held(m_server.rights(readme)));
    EXPECT_EQ(held(m_server.node(copy)), m_fs_node);
    // From the start again, and the two do not share an offset.
    EXPECT_EQ(text_of(read(copy, 5).data), "hello");
    EXPECT_EQ(text_of(read(readme, 2).data), ", ");
    EXPECT_EQ(m_fs.reads.back().cookie, m_fs.cookies[0]);
    EXPECT_NE(m_fs.reads[1].cookie, m_fs.reads[0].cookie);

    // A directory's clone lists from "." again.
    auto const fs = handler_root(root);
    EXPECT_EQ(read_dirents(fs, prot::io::MAX_BUF).names.size(), 3u);
    auto const listed = clone(fs, OTHER_OBJECT);
    ASSERT_EQ(listed.action, Action::Bind);
    auto const again = read_dirents(listed.connection, prot::io::MAX_BUF).names;
    ASSERT_FALSE(again.empty());
    EXPECT_EQ(again.front(), ".");

    // The handler can refuse, and then nothing is made and nothing is
    // released.
    auto const releases = m_fs.releases.size();
    m_fs.clone_error = Status::NotReachable;
    m_server.release(copy);
    m_server.release(listed.connection);
    EXPECT_EQ(m_fs.releases.size(), releases + 2);
    auto const refused_clone = clone(readme, OTHER_OBJECT);
    EXPECT_EQ(refused_clone.action, Action::Refuse);
    EXPECT_EQ(refused_clone.handle, OTHER_OBJECT);
    EXPECT_EQ(epitaph(refused_clone), Status::NotReachable);
    EXPECT_EQ(m_fs.releases.size(), releases + 2);
    m_fs.clone_error = Status::Ok;
    // ... and the slot it did not take is free: root, readme and fs are held.
    ASSERT_EQ(clone(readme, OTHER_OBJECT).action, Action::Bind);
}

TEST_P(IoserviceServerHandler, ACloneTheHandlerRefusesDoesNotKeepItsSlot) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    // Every slot but one is taken.
    auto spare = ConnectionId{};
    for (auto bound = m_server.bind(CHANNEL, m_root, prot::io::R_STAR_DIR); bound.is_ok();
         bound = m_server.bind(CHANNEL, m_root, prot::io::R_STAR_DIR)) {
        spare = bound.unwrap();
    }
    m_server.release(spare);

    // The refusals take no slot ...
    m_fs.clone_error = Status::NotReachable;
    for (auto count = 0; count < 3; ++count) {
        EXPECT_EQ(epitaph(clone(readme, OTHER_OBJECT)), Status::NotReachable);
    }
    m_fs.clone_error = Status::Ok;
    // ... so the one that is left is there for a clone that works.
    EXPECT_EQ(clone(readme, OTHER_OBJECT).action, Action::Bind);
    EXPECT_EQ(m_fs.bad_releases, 0u);
}

TEST_P(IoserviceServerHandler, AFullTableRefusesAnOpenAndACloneAndLeavesNoCookieBehind) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    fill_table();

    // The handler is not asked to clone for a connection that cannot be made.
    auto const cloned = clone(readme, OTHER_OBJECT);
    EXPECT_EQ(cloned.action, Action::Refuse);
    EXPECT_EQ(epitaph(cloned), Status::NoResource);
    EXPECT_TRUE(m_fs.clones.empty());

    // An Open reaches the handler, which cannot know, and the cookie it made
    // goes back once the Server finds no room.
    EXPECT_EQ(refused(root, "fs/config", prot::io::PERM_READABLE), Status::NoResource);
    ASSERT_EQ(m_fs.opens.size(), 2u);
    EXPECT_EQ(m_fs.live.size(), 1u);
    EXPECT_EQ(m_fs.releases.size(), 1u);
    EXPECT_EQ(m_fs.releases[0], (std::pair<u64, u64>{FS, m_fs.cookies[1]}));
    EXPECT_EQ(m_fs.bad_releases, 0u);
}

TEST_P(IoserviceServerHandler, ReleaseIsCalledOnceForEveryEndOfAConnection) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    // Four connections to the handler's objects.
    auto const closed = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    auto const released = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    EXPECT_EQ(m_fs.live.size(), 2u);

    // Node.Close: answered, then released.
    auto const outcome = dispatch(closed, prot::io::encode_close(m_bytes, {.txid = TXID}));
    EXPECT_EQ(outcome.action, Action::ReplyAndClose);
    ASSERT_EQ(m_fs.releases.size(), 1u);
    EXPECT_EQ(m_fs.releases[0], (std::pair<u64, u64>{FS, m_fs.cookies[0]}));

    // The peer closed, or the program could not wait on it.
    m_server.release(released);
    ASSERT_EQ(m_fs.releases.size(), 2u);
    EXPECT_EQ(m_fs.releases[1], (std::pair<u64, u64>{FS, m_fs.cookies[1]}));

    // A stale id, from either end, does not tell the handler again.
    m_server.release(closed);
    m_server.release(released);
    EXPECT_EQ(read_dirents_outcome(closed), Action::Drop);
    EXPECT_EQ(m_fs.releases.size(), 2u);

    // A message that ends the connection ends the object as well.
    auto const malformed = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    auto const dropped = dispatch(
        malformed, prot::io::encode_read_dirents(m_bytes, {.txid = TXID, .max_bytes = 10}));
    EXPECT_EQ(dropped.action, Action::Drop);
    EXPECT_EQ(m_fs.releases.size(), 3u);

    // A connection that is not a handler's tells it nothing.
    m_server.release(root);
    EXPECT_EQ(m_fs.releases.size(), 3u);
    EXPECT_TRUE(m_fs.live.empty());
    EXPECT_EQ(m_fs.bad_releases, 0u);
    EXPECT_EQ(m_fs.bad_calls, 0u);
}

TEST_P(IoserviceServerHandler, AHandlerThatMisbehavesGetsAnInternalErrorAndNothingMoves) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
    auto const fs = handler_root(root);

    // More bytes than asked for: no reply carries them and the offset stays.
    m_fs.fault = Fault::ReadsTooMuch;
    EXPECT_EQ(read(readme, 5).status, Status::InternalError);
    m_fs.fault = Fault::None;
    EXPECT_EQ(text_of(read(readme, 5).data), "hello");

    // More dirents than there was room for: the listing starts at "." still.
    m_fs.fault = Fault::ListsTooMuch;
    EXPECT_EQ(read_dirents(fs, 100).status, Status::InternalError);
    m_fs.fault = Fault::None;
    EXPECT_EQ(read_dirents(fs, 100).names, (std::vector<std::string>{".", "bin", "config"}));

    // A VMO that is not a handle.
    m_fs.fault = Fault::ReturnsAnInvalidVmo;
    EXPECT_EQ(get_backing_memory(readme, VmoFlags::Read).status, Status::InternalError);
    m_fs.fault = Fault::None;
    EXPECT_EQ(get_backing_memory(readme, VmoFlags::Read).vmo, DUPLICATE);
}

TEST_F(IoserviceServerHandlerNode, AHandlerNodeIsListedAsADirectoryAndBindsNoConnection) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const listing = read_dirents(root, prot::io::MAX_BUF);
    EXPECT_EQ(listing.names, (std::vector<std::string>{".", "bin", "config", "fs", "other"}));
    ASSERT_EQ(listing.types.size(), 5u);
    EXPECT_EQ(listing.types[3], prot::io::DirentType::Directory);
    EXPECT_EQ(listing.types[4], prot::io::DirentType::Directory);
    EXPECT_EQ(m_server.bind(CHANNEL, m_fs_node, prot::io::R_STAR_DIR).unwrap_err(), Status::NotDir);
}

TEST_F(IoserviceServerHandlerNode, ASynchronousHandlerNeverMakesAnOpenWait) {
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const outcome = open(root, "fs/config", prot::io::PERM_READABLE);
    EXPECT_EQ(outcome.action, Action::Bind);
}

TEST_F(IoserviceServerHandlerNode, WithoutAHandlerAnOpenOfAHandlerNodeIsNotSupported) {
    auto server = ioservice::Server(m_tree, m_connections, {.handle_duplicate = fake_duplicate});
    auto const root = server.bind(CHANNEL, m_root, prot::io::R_STAR_DIR).unwrap();
    auto const encoded = prot::io::encode_open(
        m_bytes, m_handles,
        {.flags = prot::io::PERM_READABLE, .path = "fs/config", .object = OBJECT});
    auto const outcome = server.dispatch(root, encoded.unwrap(), m_out);
    ASSERT_EQ(outcome.action, Action::Refuse);
    EXPECT_EQ(epitaph(outcome), Status::NotSupported);
    EXPECT_TRUE(m_fs.opens.empty());
}

// --- Requests that wait ----------------------------------------------------------------

/// The Outcome a Server::complete() ended with; a test failure, and a Drop,
/// when it was refused. unwrap() would trap, which ends the whole binary where
/// a failure should end one test.
auto soft(ustl::Result<Outcome, Status> const &result) -> Outcome {
    EXPECT_TRUE(result.is_ok());
    if (result.is_err()) {
        return Outcome{.action = Action::Drop};
    }
    return result.unwrap();
}

/// The status a Server::complete() was refused with, or Ok when it was not,
/// for the same reason.
auto refusal_of(ustl::Result<Outcome, Status> const &result) -> Status {
    if (result.is_ok()) {
        return Status::Ok;
    }
    return result.unwrap_err();
}

/// A handler that answers Pending for the operations a test chooses, and holds
/// the results back until the test ends the requests with complete().
class IoserviceServerPending: public IoserviceServerHandlerNode {
  protected:
    /// A Read request, encoded with its own txid.
    auto read_request(u64 const count, u32 const txid = TXID) -> wire::Message {
        return prot::io::encode_read(m_bytes, {.txid = txid, .count = count}).unwrap();
    }

    /// Dispatches without settling, as a Loop callback does.
    auto send(ConnectionId const connection, wire::Message const message) -> Outcome {
        return m_server.dispatch(connection, message, m_out);
    }

    /// An Open that must be left waiting: the ticket the handler was given.
    auto send_open(ConnectionId const connection, std::string_view const path, Flags const flags)
        -> Ticket {
        auto const encoded = prot::io::encode_open(
            m_bytes, m_handles, {.flags = flags, .path = path, .object = OBJECT});
        auto const outcome = send(connection, encoded.unwrap());
        EXPECT_EQ(outcome.action, Action::Pending);
        EXPECT_FALSE(m_fs.opens.empty());
        return m_fs.opens.back().ticket;
    }

    /// A Read that must be left waiting.
    auto send_read(ConnectionId const connection, u64 const count, u32 const txid = TXID)
        -> Ticket {
        auto const outcome = send(connection, read_request(count, txid));
        EXPECT_EQ(outcome.action, Action::Pending);
        EXPECT_FALSE(m_fs.reads.empty());
        return m_fs.reads.back().ticket;
    }

    /// A file connection to README, opened while no operation waits.
    auto readme_file(ConnectionId const root) -> ConnectionId {
        auto const pend = std::exchange(m_fs.pend, u8{0});
        auto const readme = opened(root, "fs/config/readme", prot::io::PERM_READABLE);
        m_fs.pend = pend;
        return readme;
    }

    auto readme_bytes(std::string_view const text) -> ReadResult {
        return ReadResult{ustl::ok(bytes_of(text))};
    }

    /// Fills the shared buffer with marks, so that a call that touches it shows.
    auto mark_out() -> void {
        m_out.bytes.fill(FILL);
        m_out.handles.fill(SENTINEL);
    }

    [[nodiscard]]
    auto out_is_marked() const -> bool {
        return std::ranges::all_of(m_out.bytes, [](u8 const b) { return b == FILL; }) &&
               m_out.handles[0] == SENTINEL;
    }

    static auto reply_of(Outcome const &outcome, u32 const txid = TXID) -> prot::io::ReadReply {
        EXPECT_EQ(outcome.action, Action::Reply);
        auto const reply = prot::io::decode_read_reply(outcome.message, txid);
        EXPECT_TRUE(reply.is_ok());
        if (reply.is_err()) {
            return {.status = Status::InternalError};
        }
        return reply.unwrap();
    }
};

TEST_F(IoserviceServerPending, APendingReadIsHeldUntilItIsCompleted) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);

    // The handler is told the connection's ticket, and the Server writes
    // nothing: the outcome only says to wait.
    auto const ticket = send_read(readme, 5);
    EXPECT_EQ(ticket.connection, readme);
    EXPECT_EQ(ticket.serial, 1u);
    EXPECT_EQ(m_fs.reads[0].offset, 0u);
    ASSERT_EQ(m_fs.parked.size(), 1u);

    auto const completed = m_server.complete(ticket, readme_bytes("hello"), m_out);
    ASSERT_TRUE(completed.is_ok());
    EXPECT_EQ(completed->action, Action::Reply);
    auto const reply = reply_of(soft(completed));
    EXPECT_EQ(reply.status, Status::Ok);
    EXPECT_EQ(text_of(reply.data), "hello");

    // The connection reads again: the next request is a new one, with the
    // offset the completed Read moved it to.
    auto const next = send_read(readme, 3);
    EXPECT_EQ(next.serial, 2u);
    EXPECT_EQ(m_fs.reads[1].offset, 5u);
}

TEST_F(IoserviceServerPending, OnlyACompletedReadMovesTheOffset) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);

    // Pending moves nothing: the next request, behind a failed completion,
    // starts where the first did.
    auto const first = send_read(readme, 100);
    ASSERT_TRUE(
        m_server.complete(first, ReadResult{ustl::err(Status::NotReachable)}, m_out).is_ok());
    auto const second = send_read(readme, 100);
    EXPECT_EQ(m_fs.reads[1].offset, 0u);
    // A completion moves it by the bytes it carried, once, and by no more
    // than that: not by the count that was asked.
    auto const completed = m_server.complete(second, readme_bytes("hello"), m_out);
    EXPECT_EQ(reply_of(soft(completed)).status, Status::Ok);
    send_read(readme, 100);
    EXPECT_EQ(m_fs.reads[2].offset, 5u);
}

TEST_F(IoserviceServerPending, AFailedCompletionIsTheStatusFormAndMovesNothing) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);
    auto const ticket = send_read(readme, 5);
    auto const completed =
        m_server.complete(ticket, ReadResult{ustl::err(Status::NotReachable)}, m_out);
    ASSERT_TRUE(completed.is_ok());
    EXPECT_EQ(reply_of(soft(completed)).status, Status::NotReachable);

    // The handler returning more than was asked is the handler's bug.
    auto const greedy = send_read(readme, 2);
    auto const refused_bytes = m_server.complete(greedy, readme_bytes("hello"), m_out);
    ASSERT_TRUE(refused_bytes.is_ok());
    EXPECT_EQ(reply_of(soft(refused_bytes)).status, Status::InternalError);
    send_read(readme, 5);
    EXPECT_EQ(m_fs.reads.back().offset, 0u);
}

TEST_F(IoserviceServerPending, APendingOpenHoldsTheServerEndAndIsAnsweredWhenItCompletes) {
    m_fs.pend = PEND_OPEN;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const flags = Flags::ProtocolFile | prot::io::PERM_READABLE;
    auto const ticket = send_open(root, "fs/config/readme", flags);

    // No connection exists yet, and the Open's server end is not in the
    // outcome: the Server holds it.
    EXPECT_TRUE(
        m_server.channel(ConnectionId{.index = 1, .generation = ioservice::FIRST_GENERATION})
            .is_none());
    EXPECT_EQ(ticket.connection, root);

    auto const completed = m_fs.complete(m_server, m_out, ticket);
    ASSERT_TRUE(completed.is_ok());
    EXPECT_EQ(completed->action, Action::Bind);
    // The server end comes back with the connection made on it, whose rights
    // are the ones the Open's flags asked for.
    EXPECT_EQ(completed->handle, OBJECT);
    EXPECT_EQ(held(m_server.channel(completed->connection)), OBJECT);
    EXPECT_EQ(held(m_server.rights(completed->connection)),
              Operations::ReadBytes | Operations::GetAttributes);
    EXPECT_EQ(held(m_server.node(completed->connection)), m_fs_node);
    // The directory connection the Open came on reads again.
    EXPECT_EQ(
        m_server
            .dispatch(
                root,
                prot::io::encode_read_dirents(m_bytes, {.txid = TXID, .max_bytes = 20}).unwrap(),
                m_out)
            .action,
        Action::Reply);
}

TEST_F(IoserviceServerPending, ARefusalOfAPendingOpenIsAnEpitaphOnTheHeldServerEnd) {
    m_fs.pend = PEND_OPEN;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);

    // The handler refuses.
    auto const missing = send_open(root, "fs/nope", prot::io::PERM_READABLE);
    auto const refusal = m_fs.complete(m_server, m_out, missing);
    ASSERT_TRUE(refusal.is_ok());
    EXPECT_EQ(refusal->action, Action::Refuse);
    EXPECT_EQ(refusal->handle, OBJECT);
    EXPECT_EQ(refusal->status, Status::NotFound);
    EXPECT_EQ(epitaph(soft(refusal)), Status::NotFound);
    EXPECT_TRUE(refusal->path.is_none());

    // The Server refuses what the handler opened, by the Open's own flags:
    // a file for a request that accepts only directories.
    auto const wrong_kind =
        send_open(root, "fs/config/readme", Flags::ProtocolDirectory | prot::io::PERM_READABLE);
    auto const not_dir = m_fs.complete(m_server, m_out, wrong_kind);
    ASSERT_TRUE(not_dir.is_ok());
    EXPECT_EQ(not_dir->action, Action::Refuse);
    EXPECT_EQ(epitaph(soft(not_dir)), Status::NotDir);
    // ... and the object it opened is let go.
    EXPECT_TRUE(m_fs.live.empty());
    EXPECT_EQ(m_fs.releases.size(), 1u);

    // The table filled up while it waited.
    auto const late = send_open(root, "fs/config", prot::io::PERM_READABLE);
    fill_table();
    auto const full = m_fs.complete(m_server, m_out, late);
    ASSERT_TRUE(full.is_ok());
    EXPECT_EQ(full->action, Action::Refuse);
    EXPECT_EQ(epitaph(soft(full)), Status::NoResource);
    EXPECT_TRUE(m_fs.live.empty());
    EXPECT_EQ(m_fs.releases.size(), 2u);
    EXPECT_EQ(m_fs.bad_releases, 0u);
}

TEST_F(IoserviceServerPending, AMessageForAPendingConnectionDropsItAndReleasesIt) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);
    auto const ticket = send_read(readme, 5);

    auto const dropped = send(readme, read_request(5));
    EXPECT_EQ(dropped.action, Action::Drop);
    EXPECT_EQ(dropped.handle, Uandle::Invalid);
    EXPECT_TRUE(m_server.channel(readme).is_none());
    ASSERT_EQ(m_fs.releases.size(), 1u);
    EXPECT_EQ(m_fs.releases[0], (std::pair<u64, u64>{FS, m_fs.cookies[0]}));
    // The handler is not asked a second time, and the ticket names nothing.
    EXPECT_EQ(m_fs.reads.size(), 1u);
    mark_out();
    EXPECT_EQ(refusal_of(m_server.complete(ticket, readme_bytes("hello"), m_out)),
              Status::NotFound);
    EXPECT_TRUE(out_is_marked());
}

TEST_F(IoserviceServerPending, DroppingAConnectionWithAPendingOpenHandsTheServerEndBack) {
    m_fs.pend = PEND_OPEN;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const ticket = send_open(root, "fs/config", prot::io::PERM_READABLE);

    auto const dropped = send(root, read_request(5));
    EXPECT_EQ(dropped.action, Action::Drop);
    // The Open's server end was the Server's to hold, and nothing will
    // answer it: it comes back with the Drop, to be closed.
    EXPECT_EQ(dropped.handle, OBJECT);
    EXPECT_TRUE(m_server.channel(root).is_none());
    EXPECT_EQ(refusal_of(m_server.complete(ticket, OpenResult{ustl::err(Status::NotFound)}, m_out)),
              Status::NotFound);

    // The handler had made the object the Open named. A completion that is
    // refused takes nothing of it: the cookie is still the handler's, and the
    // Server has never heard of it, so it does not release it.
    ASSERT_EQ(m_fs.live.size(), 1u);
    EXPECT_EQ(refusal_of(m_fs.complete(m_server, m_out, ticket)), Status::NotFound);
    EXPECT_EQ(m_fs.live.size(), 1u);
    EXPECT_TRUE(m_fs.releases.empty());
}

TEST_F(IoserviceServerPending, ReleasingAConnectionWithAPendingOpenAnswersTheServerEnd) {
    m_fs.pend = PEND_OPEN;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    send_open(root, "fs/config", prot::io::PERM_READABLE);
    EXPECT_EQ(m_server.release(root), OBJECT);
    // Released once, for what is gone: nothing is left to give back.
    EXPECT_EQ(m_server.release(root), Uandle::Invalid);
}

TEST_F(IoserviceServerPending, AStaleGenerationIsRefusedAndTheNewConnectionIsLeftAlone) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const old_readme = readme_file(root);
    auto const old_ticket = send_read(old_readme, 5);
    m_server.release(old_readme);

    // The slot is another connection now, waiting on a Read of its own whose
    // ticket has the very same serial.
    auto const readme = readme_file(root);
    ASSERT_EQ(readme.index, old_readme.index);
    ASSERT_NE(readme.generation, old_readme.generation);
    auto const ticket = send_read(readme, 3);
    EXPECT_EQ(ticket.serial, old_ticket.serial);

    mark_out();
    EXPECT_EQ(refusal_of(m_server.complete(old_ticket, readme_bytes("hello"), m_out)),
              Status::NotFound);
    EXPECT_TRUE(out_is_marked());
    // The connection that has the slot now still waits for its own answer.
    auto const completed = m_server.complete(ticket, readme_bytes("hel"), m_out);
    ASSERT_TRUE(completed.is_ok());
    EXPECT_EQ(text_of(reply_of(soft(completed)).data), "hel");
}

TEST_F(IoserviceServerPending, AnEarlierSerialIsRefusedAfterALaterRequestBegan) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);

    auto const first = send_read(readme, 5);
    ASSERT_TRUE(m_server.complete(first, readme_bytes("hello"), m_out).is_ok());
    auto const second = send_read(readme, 3);
    EXPECT_EQ(first.serial, 1u);
    EXPECT_EQ(second.serial, 2u);
    EXPECT_EQ(first.connection, second.connection);

    // The connection waits, but not for the first request.
    mark_out();
    EXPECT_EQ(refusal_of(m_server.complete(first, readme_bytes("again"), m_out)), Status::NotFound);
    EXPECT_TRUE(out_is_marked());
    // The later one is still good, with its own answer.
    auto const completed = m_server.complete(second, readme_bytes(", w"), m_out);
    ASSERT_TRUE(completed.is_ok());
    EXPECT_EQ(text_of(reply_of(soft(completed)).data), ", w");
}

TEST_F(IoserviceServerPending, EveryRequestTheHandlerIsAskedAboutGetsASerialOfItsOwn) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);

    // A Read the handler answers at once still takes a number, so a ticket
    // from before it cannot be taken for the one after.
    auto const first = send_read(readme, 5);
    ASSERT_TRUE(m_server.complete(first, readme_bytes("hello"), m_out).is_ok());
    m_fs.pend = 0;
    EXPECT_EQ(send(readme, read_request(3)).action, Action::Reply);
    m_fs.pend = PEND_READ;
    auto const third = send_read(readme, 3);
    EXPECT_EQ(third.serial, 3u);
    EXPECT_EQ(refusal_of(m_server.complete(first, readme_bytes("x"), m_out)), Status::NotFound);
}

TEST_F(IoserviceServerPending, ATicketCompletesOnceAndTheSecondTimeIsRefused) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);
    auto const ticket = send_read(readme, 5);

    ASSERT_TRUE(m_server.complete(ticket, readme_bytes("hello"), m_out).is_ok());
    mark_out();
    EXPECT_EQ(refusal_of(m_server.complete(ticket, readme_bytes("hello"), m_out)),
              Status::NotFound);
    EXPECT_TRUE(out_is_marked());
    // The offset moved once.
    send_read(readme, 3);
    EXPECT_EQ(m_fs.reads.back().offset, 5u);
}

TEST_F(IoserviceServerPending, ATicketOfARequestThatNeverWaitedIsRefused) {
    m_fs.pend = 0;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);
    EXPECT_EQ(send(readme, read_request(5)).action, Action::Reply);
    ASSERT_EQ(m_fs.reads.size(), 1u);
    EXPECT_EQ(refusal_of(m_server.complete(m_fs.reads[0].ticket, readme_bytes("hello"), m_out)),
              Status::NotFound);
    // Nor does a ticket nobody was given.
    EXPECT_EQ(refusal_of(m_server.complete(Ticket{}, readme_bytes("hello"), m_out)),
              Status::NotFound);
}

TEST_F(IoserviceServerPending, ATicketOfAReleasedConnectionIsRefused) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);
    auto const ticket = send_read(readme, 5);
    m_server.release(readme);
    ASSERT_EQ(m_fs.releases.size(), 1u);

    mark_out();
    EXPECT_EQ(refusal_of(m_server.complete(ticket, readme_bytes("hello"), m_out)),
              Status::NotFound);
    EXPECT_TRUE(out_is_marked());
    // The handler is told once, and the late completion does not tell it again.
    EXPECT_EQ(m_fs.releases.size(), 1u);
}

TEST_F(IoserviceServerPending, AResultOfAnotherOperationIsRefusedAndTheRequestStaysPending) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);
    auto const ticket = send_read(readme, 5);

    mark_out();
    EXPECT_EQ(refusal_of(
                  m_server.complete(ticket, EntriesResult{ustl::err(Status::NotReachable)}, m_out)),
              Status::InvArgs);
    EXPECT_EQ(refusal_of(m_server.complete(ticket, VmoResult{ustl::ok(VMO_X)}, m_out)),
              Status::InvArgs);
    EXPECT_EQ(refusal_of(m_server.complete(ticket, OpenResult{ustl::err(Status::NotFound)}, m_out)),
              Status::InvArgs);
    EXPECT_TRUE(out_is_marked());
    // It is still waiting for its own kind of answer.
    auto const completed = m_server.complete(ticket, readme_bytes("hello"), m_out);
    ASSERT_TRUE(completed.is_ok());
    EXPECT_EQ(text_of(reply_of(soft(completed)).data), "hello");
}

TEST_F(IoserviceServerPending, ARefusedCompletionLeavesTheVmoHandleToTheCaller) {
    m_fs.pend = PEND_VMO;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);
    auto const outcome =
        send(readme,
             prot::io::encode_get_backing_memory(m_bytes, {.txid = TXID, .flags = VmoFlags::Read})
                 .unwrap());
    ASSERT_EQ(outcome.action, Action::Pending);
    auto const ticket = m_fs.vmos[0].ticket;

    // A result of another operation: the handle is not taken.
    mark_out();
    EXPECT_EQ(refusal_of(m_server.complete(ticket, readme_bytes("x"), m_out)), Status::InvArgs);
    EXPECT_TRUE(out_is_marked());

    // Accepted: the handle is in the reply, for the program to write.
    auto const completed = m_server.complete(ticket, VmoResult{ustl::ok(VMO_X)}, m_out);
    ASSERT_TRUE(completed.is_ok());
    EXPECT_EQ(completed->action, Action::Reply);
    ASSERT_EQ(completed->message.handles.size(), 1u);
    EXPECT_EQ(completed->message.handles[0], VMO_X);
    EXPECT_EQ(m_out.handles[0], VMO_X);

    // A duplicate of the spent ticket: refused, the handle left where it
    // was, and nothing written for it.
    mark_out();
    EXPECT_EQ(refusal_of(m_server.complete(ticket, VmoResult{ustl::ok(VMO_README)}, m_out)),
              Status::NotFound);
    EXPECT_TRUE(out_is_marked());

    // A connection that is gone: the same.
    auto const second =
        send(readme,
             prot::io::encode_get_backing_memory(m_bytes, {.txid = TXID, .flags = VmoFlags::Read})
                 .unwrap());
    ASSERT_EQ(second.action, Action::Pending);
    auto const ticket_two = m_fs.vmos[1].ticket;
    m_server.release(readme);
    mark_out();
    EXPECT_EQ(refusal_of(m_server.complete(ticket_two, VmoResult{ustl::ok(VMO_README)}, m_out)),
              Status::NotFound);
    EXPECT_TRUE(out_is_marked());
}

TEST_F(IoserviceServerPending, TwoConnectionsWaitSideBySideAndEndInAnyOrder) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);
    auto const pend = std::exchange(m_fs.pend, u8{0});
    auto const x = opened(root, "fs/bin/x", prot::io::PERM_READABLE);
    m_fs.pend = pend;

    auto const first = send_read(readme, 5, 7);
    auto const second = send_read(x, 3, 8);
    EXPECT_NE(first.connection, second.connection);

    // The later request ends first, with its own txid and bytes.
    auto const second_done = m_server.complete(second,
                                               readme_bytes("\x7f"
                                                            "EL"),
                                               m_out);
    ASSERT_TRUE(second_done.is_ok());
    EXPECT_EQ(text_of(reply_of(soft(second_done), 8).data), "\x7f"
                                                            "EL");
    auto const first_done = m_server.complete(first, readme_bytes("hello"), m_out);
    ASSERT_TRUE(first_done.is_ok());
    EXPECT_EQ(text_of(reply_of(soft(first_done), 7).data), "hello");
}

TEST_F(IoserviceServerPending, APendingListingIsCompletedWithTheDotAndTheCursor) {
    m_fs.pend = PEND_DIRENTS;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const pend = std::exchange(m_fs.pend, u8{0});
    auto const fs = handler_root(root);
    m_fs.pend = pend;

    auto const listing =
        prot::io::encode_read_dirents(m_bytes, {.txid = TXID, .max_bytes = prot::io::MAX_BUF});
    ASSERT_EQ(send(fs, listing.unwrap()).action, Action::Pending);
    ASSERT_EQ(m_fs.listings.size(), 1u);
    EXPECT_EQ(m_fs.listings[0].cursor, 0u);
    // The room the handler is given leaves the "." its place.
    EXPECT_EQ(m_fs.listings[0].max_bytes, prot::io::MAX_BUF - 11);
    auto const completed = m_fs.complete(m_server, m_out, m_fs.listings[0].ticket);
    ASSERT_TRUE(completed.is_ok());
    ASSERT_EQ(completed->action, Action::Reply);
    auto const reply = prot::io::decode_read_dirents_reply(completed->message, TXID);
    ASSERT_TRUE(reply.is_ok());
    auto names = std::vector<std::string>{};
    auto reader = prot::io::DirentReader(reply->dirents);
    for (auto next = reader.next(); next.is_ok() && next->is_some(); next = reader.next()) {
        names.emplace_back(next.unwrap().value().name);
    }
    EXPECT_EQ(names, (std::vector<std::string>{".", "bin", "config"}));

    // The cursor the completion named is the next call's; the dot is gone.
    auto const again = prot::io::encode_read_dirents(m_bytes, {.txid = TXID, .max_bytes = 100});
    ASSERT_EQ(send(fs, again.unwrap()).action, Action::Pending);
    EXPECT_EQ(m_fs.listings[1].cursor, 2u);
    EXPECT_EQ(m_fs.listings[1].max_bytes, 100u);

    // A listing that fails leaves the cursor and the dot.
    ASSERT_TRUE(m_server
                    .complete(m_fs.listings[1].ticket,
                              EntriesResult{ustl::err(Status::NotReachable)}, m_out)
                    .is_ok());
    ASSERT_EQ(send(fs, again.unwrap()).action, Action::Pending);
    EXPECT_EQ(m_fs.listings[2].cursor, 2u);
}

TEST_F(IoserviceServerPending, AHandlerThatPromisesAnotherTicketIsRefusedAndNothingWaits) {
    m_fs.pend = PEND_READ | PEND_OPEN;
    m_fs.fault = Fault::PromisesAnotherTicket;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);

    // An Open: the epitaph is the answer, and the directory is not waiting.
    auto const encoded = prot::io::encode_open(
        m_bytes, m_handles,
        {.flags = prot::io::PERM_READABLE, .path = "fs/config", .object = OBJECT});
    auto const refusal = send(root, encoded.unwrap());
    ASSERT_EQ(refusal.action, Action::Refuse);
    EXPECT_EQ(epitaph(refusal), Status::InternalError);

    m_fs.fault = Fault::None;
    m_fs.pend = 0;
    auto const readme = readme_file(root);
    m_fs.pend = PEND_READ;
    m_fs.fault = Fault::PromisesAnotherTicket;
    auto const answer = send(readme, read_request(5));
    EXPECT_EQ(reply_of(answer).status, Status::InternalError);
    m_fs.fault = Fault::None;
    m_fs.pend = 0;
    EXPECT_EQ(text_of(reply_of(send(readme, read_request(5))).data), "hello");
}

TEST_F(IoserviceServerPending, ACompletionInsideTheCallThatGotTheTicketIsRefused) {
    m_fs.pend = PEND_READ;
    m_fs.fault = Fault::CompletesInsideTheCall;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    m_fs.pend = 0;
    auto const readme = readme_file(root);
    m_fs.pend = PEND_READ;

    auto const ticket = send_read(readme, 5);
    // The connection did not wait yet while the handler's call ran.
    EXPECT_EQ(m_fs.completed_inside, Status::NotFound);
    // Afterwards it does, and the same ticket is good.
    auto const completed = m_server.complete(ticket, readme_bytes("hello"), m_out);
    ASSERT_TRUE(completed.is_ok());
    EXPECT_EQ(text_of(reply_of(soft(completed)).data), "hello");
}

TEST_F(IoserviceServerPending, ReleaseIsCalledOnceForAConnectionThatEndsWhilePending) {
    m_fs.pend = PEND_READ;
    auto const root = bind(m_root, prot::io::R_STAR_DIR);
    auto const readme = readme_file(root);
    auto const ticket = send_read(readme, 5);

    m_server.release(readme);
    EXPECT_EQ(m_fs.releases.size(), 1u);
    // The handler finishes anyway, as it must, and is told it was too late.
    EXPECT_EQ(refusal_of(m_server.complete(ticket, readme_bytes("hello"), m_out)),
              Status::NotFound);
    m_server.release(readme);
    EXPECT_EQ(m_fs.releases.size(), 1u);
    EXPECT_TRUE(m_fs.live.empty());
    EXPECT_EQ(m_fs.bad_releases, 0u);
}
} // namespace
