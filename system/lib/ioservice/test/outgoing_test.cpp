/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// ioservice::Outgoing on a real Loop, against the fake kernel of fake_kernel.hpp:
// what add_protocol(), add_directory() and serve() refuse, and what each kind
// of message on the outgoing directory comes to -- an Open of svc/<name> handed
// to that protocol's handler, an Open of a name it does not serve answered
// NotFound and said on the debug console, a directory opened through it, a
// malformed message and a client gone. And a directory capability's tree at
// its declared path: the Open the root writes for it, the rights it keeps,
// and the connections and nodes that run out. And a service node inside that
// tree (add_service()): the handler an Open of it reaches, the table it shares
// with the protocols, and what it refuses. And a Subtree node at a declared
// path (add_subtree()): the Open it hands to the program's handler with the
// rest of the path and the flags, what is checked before that, the table it
// shares with the protocols and the service nodes, and what it refuses. And a
// Handler node at a declared path (add_handler()): the NodeHandler an Open
// reaches and every request on the connection it makes, the value each node is
// told, the table all four kinds of handler share, what it refuses, and
// complete() ending a request the handler answered Pending.
#include <ioservice/outgoing.hpp>

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fake_kernel.hpp"
#include <gtest/gtest.h>
#include <loop/loop.hpp>
#include <ours/port.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <prot/io.hpp>
#include <wire/wire.hpp>

// How gtest prints a status in a failure: by name, found by argument-
// dependent lookup in its own namespace. gtest looks for exactly the name
// PrintTo, hence the NOLINT.
namespace ours {
// NOLINTNEXTLINE(readability-identifier-naming): gtest's customisation point.
auto PrintTo(Status const status, std::ostream *const out) -> void {
    *out << to_string(status);
}
} // namespace ours

namespace {
using ioservice::NodeId;
using ours::Status;
using ours::Uandle;
using prot::io::Flags;

constexpr auto ECHO = std::string_view{"ours.test.Echo"};
constexpr auto OTHER = std::string_view{"ours.test.Other"};

/// What a use that asks for r* comes to as an Open's flags: the rights, and
/// the protocol bit that makes a file or a service answer NotDir. The root
/// writes this, with the declared path and the joined subdir, for each
/// directory use that routes to a provider.
constexpr auto READABLE_DIRECTORY = Flags::ProtocolDirectory | prot::io::PERM_READABLE;

/// The tests use the decided sizes as numbers, not through the constants, so
/// that a constant changed by mistake fails them instead of moving them.
constexpr auto DECIDED_CONNECTIONS = usize{32};
constexpr auto DECIDED_NODES = usize{32};

/// How many handlers the one table holds: protocols, service nodes and Subtree
/// nodes together.
constexpr auto DECIDED_HANDLERS = usize{8};

/// The root and svc/, which every outgoing directory has, are the nodes no
/// declared directory can use.
constexpr auto NODES_FOR_DIRECTORIES = DECIDED_NODES - 2;

constexpr auto HELLO = std::string_view{"hello\n"};
constexpr auto NESTED = std::string_view{"nested\n"};

/// A file serving |contents|, whose VMO is a handle nothing here uses: a Read
/// copies from the bytes.
auto file_of(std::string_view const contents) -> ioservice::VmoFile {
    return ioservice::VmoFile{
        .vmo = Uandle{0x2003},
        .bytes = {reinterpret_cast<u8 const *>(contents.data()), contents.size()},
        .executable = false,
    };
}

/// The status |result| was refused with, or Ok when it was not. unwrap_err()
/// would trap on a call that succeeded, which ends the whole test run in place
/// of failing the one test that expected a refusal.
auto refusal(ustl::Result<NodeId, Status> const &result) -> Status {
    if (result.is_ok()) {
        return Status::Ok;
    }
    return result.unwrap_err();
}

/// A protocol's handler, which keeps the channels it was handed, in order.
class Calls final: public ioservice::Handler {
  public:
    std::vector<Uandle> channels;

    auto connect(Uandle const channel) -> void override {
        channels.push_back(channel);
    }
};

/// A subtree handler, which keeps what each Open that reached it brought, in
/// order: a copy of the rest, since its view lasts for the call only.
class SubtreeCalls final: public ioservice::SubtreeHandler {
  public:
    struct Open {
        std::string rest;
        Flags flags;
        Uandle channel;
    };

    std::vector<Open> opens;

    auto open(std::string_view const rest, Flags const flags, Uandle const channel)
        -> void override {
        opens.push_back({.rest = std::string{rest}, .flags = flags, .channel = channel});
    }
};

/// A NodeHandler serving, for every Open it is asked, a directory (cookie
/// DIRECTORY_COOKIE) or, for the path "file", a file (FILE_COOKIE). It keeps
/// what each call brought, in order, and the value it was told, so that a test
/// can say which node a call was for; a Read it answers Pending when |pend_read|
/// says so, and the test ends it with Outgoing::complete().
class NodeCalls final: public ioservice::NodeHandler {
  public:
    static constexpr auto DIRECTORY_COOKIE = u64{1};
    static constexpr auto FILE_COOKIE = u64{2};
    /// What clone() adds to the cookie, so that the new connection's differs.
    static constexpr auto CLONE_OFFSET = u64{100};

    struct Open {
        u64 value;
        ustl::Option<u64> directory;
        std::string path;
        Flags flags;
    };

    struct Read {
        u64 value;
        u64 cookie;
        u64 offset;
        u64 count;
        ioservice::Ticket ticket;
    };

    struct Pair {
        u64 value;
        u64 cookie;
    };

    bool pend_read = false;

    std::vector<Open> opens;
    std::vector<Read> reads;
    std::vector<Pair> listings;
    std::vector<Pair> vmos;
    std::vector<Pair> clones;
    std::vector<Pair> releases;

    auto open(u64 const value, ustl::Option<u64> const directory, std::string_view const path,
              Flags const flags, ioservice::Ticket)
        -> ioservice::Answer<ioservice::OpenResult> override {
        opens.push_back({value, directory, std::string{path}, flags});
        auto kind = ioservice::ObjectKind::Directory;
        auto cookie = DIRECTORY_COOKIE;
        if (path == "file") {
            kind = ioservice::ObjectKind::File;
            cookie = FILE_COOKIE;
        }
        return ioservice::Done<ioservice::OpenResult>{ustl::ok(ioservice::Opened{kind, cookie})};
    }

    auto read(u64 const value, u64 const cookie, u64 const offset, u64 const count,
              ioservice::Ticket const ticket) -> ioservice::Answer<ioservice::ReadResult> override {
        reads.push_back({value, cookie, offset, count, ticket});
        if (pend_read) {
            return ioservice::Pending{ticket};
        }
        return ioservice::Done<ioservice::ReadResult>{ustl::ok(std::span<u8 const>{DATA})};
    }

    auto read_dirents(u64 const value, u64 const cookie, u64, u64, ioservice::Ticket)
        -> ioservice::Answer<ioservice::EntriesResult> override {
        listings.push_back({value, cookie});
        return ioservice::Done<ioservice::EntriesResult>{
            ustl::ok(ioservice::Entries{.dirents = {}, .next = 0})};
    }

    auto get_backing_memory(u64 const value, u64 const cookie, prot::io::VmoFlags,
                            ioservice::Ticket) -> ioservice::Answer<ioservice::VmoResult> override {
        vmos.push_back({value, cookie});
        return ioservice::Done<ioservice::VmoResult>{ustl::err(Status::NotSupported)};
    }

    auto clone(u64 const value, u64 const cookie) -> ustl::Result<u64, Status> override {
        clones.push_back({value, cookie});
        return ustl::ok(cookie + CLONE_OFFSET);
    }

    auto release(u64 const value, u64 const cookie) -> void override {
        releases.push_back({value, cookie});
    }

    /// What a Read that is answered at once returns.
    static constexpr auto DATA = std::array<u8, 7>{'h', 'a', 'n', 'd', 'l', 'e', 'r'};
};

/// A fresh fake kernel, a Loop on it, and an Outgoing on the Loop, with its
/// storage, which is too large for the stack.
class OutgoingTest: public testing::Test {
  protected:
    OutgoingTest() {
        fake::kernel().reset();
        auto port = Uandle::Invalid;
        EXPECT_EQ(ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok);
        m_storage = std::make_unique<ioservice::Outgoing::Storage>();
        m_loop.emplace(port, m_slots);
        m_outgoing.emplace(*m_storage, *m_loop);
    }

    auto outgoing() -> ioservice::Outgoing & {
        return *m_outgoing;
    }

    /// Dispatches everything queued, and checks that the port ended empty.
    auto run() -> void {
        EXPECT_EQ(m_loop->run(0), Status::TimeOut);
    }

    /// A channel pair: the client end first.
    static auto channel() -> std::array<Uandle, 2> {
        auto client = Uandle::Invalid;
        auto server = Uandle::Invalid;
        EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &client, &server),
                  Status::Ok);
        return {client, server};
    }

    /// |text|, kept for as long as the test runs, so that a path handed to the
    /// directory, whose directories' names view it, outlives the directory.
    auto keep(std::string text) -> std::string_view {
        m_texts.push_back(std::move(text));
        return m_texts.back();
    }

    /// Serves the outgoing directory and returns the client end of its
    /// DirectoryRequest channel, the end the root keeps.
    auto serve() -> Uandle {
        auto const [client, server] = channel();
        EXPECT_EQ(outgoing().serve(server), Status::Ok);
        return client;
    }

    /// Writes Open(|path|, |flags|, |object|) on |directory|.
    static auto open(Uandle const directory, std::string_view const path, Flags const flags,
                     Uandle const object) -> void {
        std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
        std::array<Uandle, 1> handles{};
        auto const encoded = prot::io::encode_open(
            bytes, handles, prot::io::OpenRequest{.flags = flags, .path = path, .object = object});
        ASSERT_TRUE(encoded.is_ok());
        auto const message = encoded.unwrap();
        EXPECT_EQ(ours::channel_write(directory, ours::ChannelWriteOptions::None,
                                      message.bytes.data(), static_cast<u32>(message.bytes.size()),
                                      message.handles.data(),
                                      static_cast<u32>(message.handles.size())),
                  Status::Ok);
    }

    /// The status of the epitaph waiting on |channel|; InvArgs when what is
    /// there is no epitaph, ShouldWait when nothing is.
    static auto epitaph(Uandle const channel) -> Status {
        std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
        std::array<Uandle, wire::MAX_MESSAGE_HANDLES> handles{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        if (auto const read =
                ours::channel_read(channel, ours::ChannelReadOptions::None, bytes.data(),
                                   handles.data(), static_cast<u32>(bytes.size()),
                                   static_cast<u32>(handles.size()), &num_bytes, &num_handles);
            read != Status::Ok) {
            return read;
        }
        auto const decoded = wire::decode_epitaph(wire::Message{
            .bytes = std::span{bytes}.first(num_bytes),
            .handles = std::span{handles}.first(num_handles),
        });
        if (decoded.is_err()) {
            return Status::InvArgs;
        }
        return decoded.unwrap();
    }

    /// An Open through |directory|, with the loop run: its answer, and the
    /// ends of the channel it carried.
    struct Opened {
        /// Ok when a connection (or a service's handler) took the channel and
        /// nothing was written on it; otherwise the status of the epitaph.
        Status status;
        Uandle client;
        Uandle server;
    };

    auto open_through(Uandle const directory, std::string_view const path, Flags const flags)
        -> Opened {
        auto const [client, server] = channel();
        open(directory, path, flags, server);
        run();
        if (fake::kernel().queued_messages(client) == 0) {
            // Nothing was written back: the channel is still the server's.
            EXPECT_TRUE(fake::kernel().is_open(server));
            return {.status = Status::Ok, .client = client, .server = server};
        }
        return {.status = epitaph(client), .client = client, .server = server};
    }

    /// The bytes a Read of |count| bytes on the file connection |file| comes
    /// back with; none when the reply is not an Ok Read reply.
    auto read_all(Uandle const file, u64 const count) -> std::optional<std::string> {
        std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
        auto const encoded =
            prot::io::encode_read(bytes, prot::io::ReadRequest{.txid = 1, .count = count});
        EXPECT_TRUE(encoded.is_ok());
        if (encoded.is_err()) {
            return std::nullopt;
        }
        auto const message = encoded.unwrap();
        EXPECT_EQ(ours::channel_write(file, ours::ChannelWriteOptions::None, message.bytes.data(),
                                      static_cast<u32>(message.bytes.size()), nullptr, 0),
                  Status::Ok);
        run();

        std::array<u8, wire::MAX_MESSAGE_BYTES> reply{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        if (ours::channel_read(file, ours::ChannelReadOptions::None, reply.data(), nullptr,
                               static_cast<u32>(reply.size()), 0, &num_bytes,
                               &num_handles) != Status::Ok) {
            return std::nullopt;
        }
        auto const decoded = prot::io::decode_read_reply(
            wire::Message{.bytes = std::span{reply}.first(num_bytes), .handles = {}}, 1);
        if (decoded.is_err() || decoded.unwrap().status != Status::Ok) {
            return std::nullopt;
        }
        auto const data = decoded.unwrap().data;
        return std::string{data.begin(), data.end()};
    }

  private:
    // A slot for each connection the directory can have, which is what a
    // program's table holds. No more: when the connections are used up, so are
    // the slots, and a test of the one does not pass for the other.
    std::array<loop::Slot, ioservice::Outgoing::MAX_CONNECTIONS> m_slots{};
    // A deque: a view of a string in it must not move when another is added.
    std::deque<std::string> m_texts;
    std::unique_ptr<ioservice::Outgoing::Storage> m_storage;
    std::optional<loop::Loop> m_loop;
    std::optional<ioservice::Outgoing> m_outgoing;
};

TEST_F(OutgoingTest, AddProtocolRefusals) {
    Calls handler;
    EXPECT_EQ(outgoing().add_protocol("", handler), Status::InvArgs);
    EXPECT_EQ(outgoing().add_protocol(".", handler), Status::InvArgs);
    EXPECT_EQ(outgoing().add_protocol("a/b", handler), Status::InvArgs);

    EXPECT_EQ(outgoing().add_protocol(ECHO, handler), Status::Ok);
    EXPECT_EQ(outgoing().add_protocol(ECHO, handler), Status::AlreadyExists);

    // MAX_PROTOCOLS in all, then no more.
    constexpr auto NAMES = std::array<std::string_view, ioservice::Outgoing::MAX_PROTOCOLS>{
        "p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8",
    };
    for (auto index = usize{1}; index < ioservice::Outgoing::MAX_PROTOCOLS; ++index) {
        EXPECT_EQ(outgoing().add_protocol(NAMES[index - 1], handler), Status::Ok);
    }
    EXPECT_EQ(outgoing().add_protocol(NAMES.back(), handler), Status::NoResource);
}

TEST_F(OutgoingTest, ServeRefusesInvalid) {
    EXPECT_EQ(outgoing().serve(Uandle::Invalid), Status::InvArgs);
}

TEST_F(OutgoingTest, OpenOfAProtocolReachesItsHandler) {
    Calls echo_calls;
    Calls other_calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, echo_calls), Status::Ok);
    ASSERT_EQ(outgoing().add_protocol(OTHER, other_calls), Status::Ok);
    auto const directory = serve();

    // What the root writes when it forwards a route into a provider:
    // svc/<name>, as a service.
    auto const [client, server] = channel();
    open(directory, "svc/ours.test.Echo", Flags::ProtocolService, server);
    run();

    ASSERT_EQ(echo_calls.channels.size(), 1u);
    EXPECT_EQ(echo_calls.channels[0], server);
    EXPECT_TRUE(other_calls.channels.empty());
    // The handler owns the channel: nothing was written on it or closed.
    EXPECT_TRUE(fake::kernel().is_open(server));
    EXPECT_EQ(fake::kernel().queued_messages(client), 0u);
    // The directory's connection stays, for the next Open.
    EXPECT_TRUE(fake::kernel().is_open(directory));

    auto const [second_client, second_server] = channel();
    open(directory, "svc/ours.test.Other", Flags::ProtocolService, second_server);
    run();
    ASSERT_EQ(other_calls.channels.size(), 1u);
    EXPECT_EQ(other_calls.channels[0], second_server);
}

TEST_F(OutgoingTest, ProtocolAddedAfterServeIsFound) {
    auto const directory = serve();
    Calls calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, calls), Status::Ok);
    auto const [client, server] = channel();
    open(directory, "svc/ours.test.Echo", Flags::ProtocolService, server);
    run();
    ASSERT_EQ(calls.channels.size(), 1u);
}

TEST_F(OutgoingTest, OpenOfAnUnservedNameAnswersNotFound) {
    Calls calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, calls), Status::Ok);
    auto const directory = serve();

    auto const [client, server] = channel();
    open(directory, "svc/ours.test.Missing", Flags::ProtocolService, server);
    run();

    EXPECT_TRUE(calls.channels.empty());
    EXPECT_EQ(epitaph(client), Status::NotFound);
    EXPECT_FALSE(fake::kernel().is_open(server));
    EXPECT_TRUE(fake::kernel().is_open(directory));
}

TEST_F(OutgoingTest, OpenOfAnUnservedNameIsSaidOnTheConsole) {
    Calls calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, calls), Status::Ok);
    auto const directory = serve();

    auto const [client, server] = channel();
    open(directory, "svc/ours.test.Missing", Flags::ProtocolService, server);
    run();

    // One line, written in one call, with the path as asked for and the
    // status the client read in its epitaph.
    EXPECT_EQ(fake::kernel().debug_writes(),
              std::vector<std::string>{"ioservice: the outgoing directory refused an Open of "
                                       "\"svc/ours.test.Missing\": NotFound\n"});
    EXPECT_EQ(epitaph(client), Status::NotFound);
}

TEST_F(OutgoingTest, TheLineSaysWhichStatusTheOpenWasRefusedWith) {
    Calls calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, calls), Status::Ok);
    auto const directory = serve();

    // A name it serves, asked for as a file.
    auto const [client, server] = channel();
    open(directory, "svc/ours.test.Echo", Flags::ProtocolFile, server);
    run();

    EXPECT_EQ(fake::kernel().debug_writes(),
              std::vector<std::string>{"ioservice: the outgoing directory refused an Open of "
                                       "\"svc/ours.test.Echo\": NotFile\n"});
    EXPECT_TRUE(calls.channels.empty());
}

TEST_F(OutgoingTest, OpensThatSucceedSayNothing) {
    Calls echo_calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, echo_calls), Status::Ok);
    auto const directory = serve();

    auto const [client, server] = channel();
    open(directory, "svc/ours.test.Echo", Flags::ProtocolService, server);
    auto const [svc_client, svc_server] = channel();
    open(directory, "svc", Flags::ProtocolDirectory | Flags::PermConnect | Flags::PermTraverse,
         svc_server);
    run();

    EXPECT_EQ(echo_calls.channels.size(), 1u);
    EXPECT_TRUE(fake::kernel().debug_writes().empty());
}

TEST_F(OutgoingTest, ALongPathIsCutToFitTheLine) {
    auto const directory = serve();
    // A valid path ours.io carries, several times what one debug_write
    // takes. Were the line not cut, the console would refuse it whole.
    auto const name = std::string(200, 'x');
    auto const [client, server] = channel();
    open(directory, "svc/" + name, Flags::ProtocolService, server);
    run();

    ASSERT_EQ(fake::kernel().debug_writes().size(), 1u);
    auto const &line = fake::kernel().debug_writes()[0];
    EXPECT_LE(line.size(), ours::MAX_DEBUG_WRITE);
    // The beginning of the path is shown, cut with "...", and the status
    // still ends the line.
    EXPECT_TRUE(line.starts_with("ioservice: the outgoing directory refused an Open of "
                                 "\"svc/xxxxxxxx"));
    EXPECT_TRUE(line.ends_with("xxx...\": NotFound\n"));
}

TEST_F(OutgoingTest, AClientCannotEndTheLineWithItsPath) {
    auto const directory = serve();
    // A name is bytes: a newline and an escape are valid in one. Quoted as
    // sent, they would end the line and write another, or drive the console.
    auto const [client, server] = channel();
    open(directory, "svc/a\nioservice: forged\x1b[2J\xff", Flags::ProtocolService, server);
    run();

    EXPECT_EQ(fake::kernel().debug_writes(),
              std::vector<std::string>{"ioservice: the outgoing directory refused an Open of "
                                       "\"svc/a?ioservice: forged?[2J?\": NotFound\n"});
}

TEST_F(OutgoingTest, ProtocolsReachedThroughAnOpenedSvcDirectory) {
    Calls calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, calls), Status::Ok);
    auto const directory = serve();

    // svc/ itself, as a directory: a connection of its own, with a slot.
    auto const [svc_client, svc_server] = channel();
    open(directory, "svc", Flags::ProtocolDirectory | Flags::PermConnect | Flags::PermTraverse,
         svc_server);
    run();
    EXPECT_TRUE(fake::kernel().is_open(svc_server));
    EXPECT_EQ(fake::kernel().queued_messages(svc_client), 0u);

    auto const [client, server] = channel();
    open(svc_client, "ours.test.Echo", Flags::ProtocolService, server);
    run();
    ASSERT_EQ(calls.channels.size(), 1u);
    EXPECT_EQ(calls.channels[0], server);
}

TEST_F(OutgoingTest, MalformedMessageClosesTheConnection) {
    auto const directory = serve();
    auto const [stray, stray_peer] = channel();
    std::array<u8, 8> garbage{};
    auto const handles = std::array{stray};
    ASSERT_EQ(ours::channel_write(directory, ours::ChannelWriteOptions::None, garbage.data(),
                                  static_cast<u32>(garbage.size()), handles.data(),
                                  static_cast<u32>(handles.size())),
              Status::Ok);
    run();

    // No epitaph, and the handle the message carried is closed with it.
    EXPECT_EQ(epitaph(directory), Status::PeerClosed);
    EXPECT_FALSE(fake::kernel().is_open(stray));
}

TEST_F(OutgoingTest, ClientGoneReleasesTheConnection) {
    // MAX_CONNECTIONS connections, each closed by its client in turn: if a
    // closed one kept its connection, the last serve() would find none free.
    for (auto round = usize{0}; round <= ioservice::Outgoing::MAX_CONNECTIONS; ++round) {
        auto const [client, server] = channel();
        ASSERT_EQ(outgoing().serve(server), Status::Ok);
        EXPECT_EQ(ours::handle_close(client), Status::Ok);
        run();
        EXPECT_FALSE(fake::kernel().is_open(server));
    }
}

TEST_F(OutgoingTest, ServeRefusesPastItsConnections) {
    std::vector<Uandle> clients;
    for (auto index = usize{0}; index < ioservice::Outgoing::MAX_CONNECTIONS; ++index) {
        clients.push_back(serve());
    }
    auto const [client, server] = channel();
    EXPECT_EQ(outgoing().serve(server), Status::NoResource);
    // Refused, so still the caller's.
    EXPECT_TRUE(fake::kernel().is_open(server));
}
// --- a directory capability at its declared path ----------------------------------

TEST_F(OutgoingTest, TheSizesAreTheDecidedOnes) {
    // Decision 8 (b) of the directory-capability design: 32 connections and
    // 32 nodes, of which the root, svc/ and a node per protocol take
    // 2 + MAX_PROTOCOLS.
    EXPECT_EQ(ioservice::Outgoing::MAX_CONNECTIONS, DECIDED_CONNECTIONS);
    EXPECT_EQ(ioservice::Outgoing::MAX_NODES, DECIDED_NODES);
    EXPECT_EQ(ioservice::Outgoing::MAX_PROTOCOLS, 8u);
}

TEST_F(OutgoingTest, AddDirectoryMakesTheDirectoryAndEveryOneOnTheWay) {
    auto const data = outgoing().add_directory("/export/data");
    ASSERT_TRUE(data.is_ok());
    auto const directory = serve();

    // Both are there, as directories, before anything is put in them.
    EXPECT_EQ(open_through(directory, "export", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "export/data", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "export/other", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(open_through(directory, "data", READABLE_DIRECTORY).status, Status::NotFound);

    // The node it returned is the last one: a file added to it is at
    // export/data/x.
    ASSERT_TRUE(outgoing().add_file(data.unwrap(), "x", file_of(HELLO)).is_ok());
    auto const x =
        open_through(directory, "export/data/x", Flags::ProtocolFile | Flags::PermReadBytes);
    ASSERT_EQ(x.status, Status::Ok);
    EXPECT_EQ(read_all(x.client, 100), std::string{HELLO});
}

TEST_F(OutgoingTest, AddDirectoryWorksBeforeAndAfterServe) {
    auto const first = outgoing().add_directory("/before");
    ASSERT_TRUE(first.is_ok());
    auto const directory = serve();
    ASSERT_TRUE(outgoing().add_directory("/after").is_ok());
    EXPECT_EQ(open_through(directory, "before", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "after", READABLE_DIRECTORY).status, Status::Ok);
}

TEST_F(OutgoingTest, SiblingsShareTheirFirstDirectory) {
    // Leaves exactly the nodes that /export/a and /export/b need if export/
    // is made once: one for each of the three, and the root and svc/.
    for (auto index = usize{0}; index < NODES_FOR_DIRECTORIES - 3; ++index) {
        ASSERT_TRUE(outgoing().add_directory(keep("/fill" + std::to_string(index))).is_ok());
    }
    auto const a = outgoing().add_directory("/export/a");
    auto const b = outgoing().add_directory("/export/b");
    ASSERT_TRUE(a.is_ok());
    ASSERT_TRUE(b.is_ok());
    EXPECT_NE(a.unwrap(), b.unwrap());
    // Nothing is left: two exports would have run out one declaration ago.
    EXPECT_EQ(refusal(outgoing().add_directory("/last")), Status::NoResource);

    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "export/a", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "export/b", READABLE_DIRECTORY).status, Status::Ok);
}

TEST_F(OutgoingTest, AddDirectoryRefusesAPathThatIsNotValid) {
    // One byte past what a path may be, in segments that are each a valid
    // name, so that the length is what is wrong with it.
    auto too_long = std::string{};
    while (too_long.size() <= prot::io::MAX_PATH_LENGTH) {
        too_long += "/a";
    }
    auto const long_path = keep(too_long);
    auto const long_name = keep("/" + std::string(prot::io::MAX_NAME_LENGTH + 1, 'x'));
    auto const refused = std::vector<std::string_view>{
        // Not absolute, or nothing below the root.
        "",
        "export/data",
        "export",
        "/",
        // An empty segment, which is how a trailing or a doubled '/' shows.
        "/export/",
        "//export",
        "/export//data",
        // "." and ".." are never a name, and a path is made of names.
        "/.",
        "/..",
        "/export/.",
        "/export/./data",
        "/export/..",
        "/export/../data",
        "/../export",
        // A NUL ends a name for a C reader.
        std::string_view{"/exp\0ort", 8},
        std::string_view{"/export/da\0ta", 13},
        long_name,
        long_path,
        // The protocols' place, by segment.
        "/svc",
        "/svc/x",
        "/svc/x/y",
        "/svc/ours.test.Echo",
    };
    for (auto const path: refused) {
        EXPECT_EQ(refusal(outgoing().add_directory(path)), Status::InvArgs) << path;
    }

    // None of them made anything, nor took a node: every directory below
    // the root is still not there, svc/ is the one that always was, and all
    // the nodes there are for directories are still free.
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "export", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(open_through(directory, "exp", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(open_through(directory, "svc", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "svc/x", READABLE_DIRECTORY).status, Status::NotFound);
    for (auto index = usize{0}; index < NODES_FOR_DIRECTORIES; ++index) {
        EXPECT_TRUE(outgoing().add_directory(keep("/d" + std::to_string(index))).is_ok()) << index;
    }
    EXPECT_EQ(refusal(outgoing().add_directory("/one-more")), Status::NoResource);
}

TEST_F(OutgoingTest, ASegmentThatOnlyBeginsLikeSvcIsNotSvc) {
    // The comparison is by segment: "svcx" and "svc2" are names of their own.
    EXPECT_TRUE(outgoing().add_directory("/svcx").is_ok());
    EXPECT_TRUE(outgoing().add_directory("/svc2/data").is_ok());
    EXPECT_TRUE(outgoing().add_directory("/export/svc").is_ok());
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "svcx", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "svc2/data", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "export/svc", READABLE_DIRECTORY).status, Status::Ok);
}

TEST_F(OutgoingTest, AddDirectoryRefusesAPathThatOverlapsADeclaredOne) {
    ASSERT_TRUE(outgoing().add_directory("/export/data").is_ok());
    ASSERT_TRUE(outgoing().add_directory("/a/b/c").is_ok());

    // Equal to one added, above it, and below it, at any depth.
    for (auto const path: {"/export/data", "/export", "/export/data/x", "/export/data/x/y",
                           "/a/b/c", "/a/b", "/a", "/a/b/c/d"}) {
        EXPECT_EQ(refusal(outgoing().add_directory(path)), Status::AlreadyExists) << path;
    }

    // By segments: a path that only shares leading bytes is a different one,
    // and a sibling is.
    for (auto const path: {"/export/data2", "/export/dat", "/export/other", "/export2/data",
                           "/a/b/d", "/a/c", "/data"}) {
        EXPECT_TRUE(outgoing().add_directory(path).is_ok()) << path;
    }

    // The refusals made nothing below the declared directory, which is the
    // program's to fill in: x is not there for it to find.
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "export/data/x", READABLE_DIRECTORY).status,
              Status::NotFound);
    EXPECT_EQ(open_through(directory, "a/b/c/d", READABLE_DIRECTORY).status, Status::NotFound);
}

TEST_F(OutgoingTest, AddDirectoryRefusedForRoomLeavesNothingBehind) {
    // One node left. /x/y needs two: x/ must not be made and then y/ refused.
    for (auto index = usize{0}; index < NODES_FOR_DIRECTORIES - 1; ++index) {
        ASSERT_TRUE(outgoing().add_directory(keep("/d" + std::to_string(index))).is_ok());
    }
    EXPECT_EQ(refusal(outgoing().add_directory("/x/y")), Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_directory("/x/y/z")), Status::NoResource);

    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "x", READABLE_DIRECTORY).status, Status::NotFound);
    // The node is still there for one that fits, and then no more.
    EXPECT_TRUE(outgoing().add_directory("/x").is_ok());
    EXPECT_EQ(open_through(directory, "x", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(refusal(outgoing().add_directory("/y")), Status::NoResource);
    // x/ is now declared, so /x/y overlaps it; room is not what it lacks.
    EXPECT_EQ(refusal(outgoing().add_directory("/x/y")), Status::AlreadyExists);
}

TEST_F(OutgoingTest, TheNodesBelowADeclaredDirectoryComeOutOfTheSamePool) {
    auto const data = outgoing().add_directory("/export/data");
    ASSERT_TRUE(data.is_ok());
    // export/ and data/ are two of the nodes for directories.
    for (auto index = usize{0}; index < NODES_FOR_DIRECTORIES - 2; ++index) {
        EXPECT_TRUE(outgoing()
                        .add_file(data.unwrap(), keep("f" + std::to_string(index)), file_of(HELLO))
                        .is_ok());
    }
    EXPECT_EQ(refusal(outgoing().add_file(data.unwrap(), "last", file_of(HELLO))),
              Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_directory(data.unwrap(), "last")), Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_directory("/other")), Status::NoResource);
}

TEST_F(OutgoingTest, TheTreeIsFilledInBelowTheDeclaredDirectory) {
    auto const data = outgoing().add_directory("/export/data").unwrap();
    auto const sub = outgoing().add_directory(data, "sub");
    ASSERT_TRUE(sub.is_ok());
    ASSERT_TRUE(outgoing().add_file(data, "x", file_of(HELLO)).is_ok());
    ASSERT_TRUE(outgoing().add_file(sub.unwrap(), "y", file_of(NESTED)).is_ok());

    // Refused as the tree refuses them, and nothing more.
    EXPECT_EQ(refusal(outgoing().add_directory(data, "")), Status::InvArgs);
    EXPECT_EQ(refusal(outgoing().add_directory(data, "a/b")), Status::InvArgs);
    EXPECT_EQ(refusal(outgoing().add_directory(data, "sub")), Status::AlreadyExists);
    EXPECT_EQ(refusal(outgoing().add_file(data, "x", file_of(HELLO))), Status::AlreadyExists);
    EXPECT_EQ(refusal(outgoing().add_file(data, "..", file_of(HELLO))), Status::InvArgs);
    // A file is no place for a child, and an id of no node is none.
    auto const x = outgoing().add_file(data, "z", file_of(HELLO)).unwrap();
    EXPECT_EQ(refusal(outgoing().add_directory(x, "below")), Status::NotDir);
    EXPECT_EQ(refusal(outgoing().add_file(NodeId{0xFFFF}, "w", file_of(HELLO))), Status::InvArgs);
}

TEST_F(OutgoingTest, AnOpenBelowTheDeclaredPathMakesAConnectionWithTheRightsAsked) {
    auto const data = outgoing().add_directory("/export/data").unwrap();
    auto const sub = outgoing().add_directory(data, "sub").unwrap();
    ASSERT_TRUE(outgoing().add_file(data, "x", file_of(HELLO)).is_ok());
    ASSERT_TRUE(outgoing().add_file(sub, "y", file_of(NESTED)).is_ok());
    auto const directory = serve();

    // What the root writes for a use at r*, with no subdir: the declared
    // path. The channel becomes a connection on the directory, held to r*.
    auto const entry = open_through(directory, "export/data", READABLE_DIRECTORY);
    ASSERT_EQ(entry.status, Status::Ok);

    // A file under it reads, through the entry and straight from the root.
    auto const through_entry =
        open_through(entry.client, "x", Flags::ProtocolFile | Flags::PermReadBytes);
    ASSERT_EQ(through_entry.status, Status::Ok);
    EXPECT_EQ(read_all(through_entry.client, 100), std::string{HELLO});
    auto const direct =
        open_through(directory, "export/data/sub/y", Flags::ProtocolFile | Flags::PermReadBytes);
    ASSERT_EQ(direct.status, Status::Ok);
    EXPECT_EQ(read_all(direct.client, 100), std::string{NESTED});

    // With a subdir, the Open names a place below the declared path, and the
    // connection is on that directory: x is not there.
    auto const subdir = open_through(directory, "export/data/sub", READABLE_DIRECTORY);
    ASSERT_EQ(subdir.status, Status::Ok);
    EXPECT_EQ(open_through(subdir.client, "y", Flags::ProtocolFile | Flags::PermReadBytes).status,
              Status::Ok);
    EXPECT_EQ(open_through(subdir.client, "x", Flags::ProtocolFile | Flags::PermReadBytes).status,
              Status::NotFound);
    // And no way out of it: a path has no "..".
    EXPECT_EQ(
        open_through(subdir.client, "../x", Flags::ProtocolFile | Flags::PermReadBytes).status,
        Status::BadPath);
}

TEST_F(OutgoingTest, AnOpenWithRightsBeyondTheConnectionsIsAccessDenied) {
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_directory(data, "sub").is_ok());
    ASSERT_TRUE(outgoing().add_file(data, "x", file_of(HELLO)).is_ok());
    auto const directory = serve();

    // An entry that holds no ENUMERATE, as a use of rx* minus it would.
    auto const entry =
        open_through(directory, "export/data",
                     Flags::ProtocolDirectory | Flags::PermReadBytes | Flags::PermTraverse);
    ASSERT_EQ(entry.status, Status::Ok);

    // What it holds, it can pass on; what it does not, it cannot.
    EXPECT_EQ(open_through(entry.client, "sub",
                           Flags::ProtocolDirectory | Flags::PermReadBytes | Flags::PermTraverse)
                  .status,
              Status::Ok);
    EXPECT_EQ(open_through(entry.client, "sub",
                           Flags::ProtocolDirectory | Flags::PermEnumerate | Flags::PermTraverse)
                  .status,
              Status::AccessDenied);
    EXPECT_EQ(open_through(entry.client, "x", Flags::ProtocolFile | Flags::PermWriteBytes).status,
              Status::AccessDenied);
    EXPECT_EQ(open_through(entry.client, "x", Flags::ProtocolFile | Flags::PermExecute).status,
              Status::AccessDenied);

    // The root's own connection holds every right, so the only ceiling on
    // the first Open, for the entry, is the route's own check: this asks for
    // what a use of w* would, and the directory serves it.
    EXPECT_EQ(
        open_through(directory, "export/data", Flags::ProtocolDirectory | prot::io::PERM_WRITABLE)
            .status,
        Status::Ok);
}

TEST_F(OutgoingTest, AFileOrAServiceOpenedAsADirectoryAnswersNotDir) {
    Calls handler;
    ASSERT_EQ(outgoing().add_protocol(ECHO, handler), Status::Ok);
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_file(data, "x", file_of(HELLO)).is_ok());
    auto const directory = serve();

    // A use whose declared path joined with its subdir ends at a file, or at
    // a service, is a directory use that cannot be served: the protocol bit
    // the root writes is what says so.
    auto const file = open_through(directory, "export/data/x", READABLE_DIRECTORY);
    EXPECT_EQ(file.status, Status::NotDir);
    EXPECT_FALSE(fake::kernel().is_open(file.server));
    auto const service = open_through(directory, "svc/ours.test.Echo", READABLE_DIRECTORY);
    EXPECT_EQ(service.status, Status::NotDir);
    EXPECT_FALSE(fake::kernel().is_open(service.server));
    // Not handed to the protocol's handler either.
    EXPECT_TRUE(handler.channels.empty());

    // Asked for as what they are, they are served.
    EXPECT_EQ(
        open_through(directory, "export/data/x", Flags::ProtocolFile | Flags::PermReadBytes).status,
        Status::Ok);
    EXPECT_EQ(open_through(directory, "svc/ours.test.Echo", Flags::ProtocolService).status,
              Status::Ok);
    EXPECT_EQ(handler.channels.size(), 1u);
}

TEST_F(OutgoingTest, ThirtyTwoConnectionsRunOutWithNoResourceOnTheOpensChannel) {
    ASSERT_TRUE(outgoing().add_directory("/export/data").is_ok());
    auto const directory = serve();

    // The root's own is one of the 32. Each entry is another, and stays
    // while its client does.
    auto entries = std::vector<Opened>{};
    for (auto index = usize{1}; index < DECIDED_CONNECTIONS; ++index) {
        entries.push_back(open_through(directory, "export/data", READABLE_DIRECTORY));
        ASSERT_EQ(entries.back().status, Status::Ok) << index;
    }
    EXPECT_TRUE(fake::kernel().debug_writes().empty());

    // The 33rd is refused, and the answer is on the channel it came with.
    auto const refused = open_through(directory, "export/data", READABLE_DIRECTORY);
    EXPECT_EQ(refused.status, Status::NoResource);
    EXPECT_FALSE(fake::kernel().is_open(refused.server));
    // The root's connection and the entries are as they were, and the
    // refusal is said, since it is the root's Open that was refused.
    EXPECT_TRUE(fake::kernel().is_open(directory));
    EXPECT_EQ(fake::kernel().debug_writes(),
              std::vector<std::string>{"ioservice: the outgoing directory refused an Open of "
                                       "\"export/data\": NoResource\n"});

    // A client that goes frees its connection for the next Open.
    EXPECT_EQ(ours::handle_close(entries.front().client), Status::Ok);
    run();
    EXPECT_EQ(open_through(directory, "export/data", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "export/data", READABLE_DIRECTORY).status,
              Status::NoResource);
}

TEST_F(OutgoingTest, ProtocolsAndDirectoriesAreServedSideBySide) {
    Calls echo_calls;
    Calls other_calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, echo_calls), Status::Ok);
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_file(data, "x", file_of(HELLO)).is_ok());
    // A protocol added after the directory.
    ASSERT_EQ(outgoing().add_protocol(OTHER, other_calls), Status::Ok);
    auto const directory = serve();

    EXPECT_EQ(open_through(directory, "svc/ours.test.Echo", Flags::ProtocolService).status,
              Status::Ok);
    EXPECT_EQ(open_through(directory, "svc/ours.test.Other", Flags::ProtocolService).status,
              Status::Ok);
    EXPECT_EQ(echo_calls.channels.size(), 1u);
    EXPECT_EQ(other_calls.channels.size(), 1u);

    auto const x =
        open_through(directory, "export/data/x", Flags::ProtocolFile | Flags::PermReadBytes);
    ASSERT_EQ(x.status, Status::Ok);
    EXPECT_EQ(read_all(x.client, 100), std::string{HELLO});

    // svc/ holds the protocols and only them; the declared directories are
    // not in it, and a protocol is not at the root's level.
    auto const svc = open_through(directory, "svc", READABLE_DIRECTORY);
    ASSERT_EQ(svc.status, Status::Ok);
    EXPECT_EQ(open_through(svc.client, "export", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(open_through(directory, "ours.test.Echo", Flags::ProtocolService).status,
              Status::NotFound);
    // And the protocol through svc/ opened as a directory is no directory.
    EXPECT_EQ(open_through(svc.client, "ours.test.Echo", READABLE_DIRECTORY).status,
              Status::NotDir);
}

TEST_F(OutgoingTest, ADeclaredPathTheProgramDoesNotServeIsSaidOnTheConsole) {
    ASSERT_TRUE(outgoing().add_directory("/export/data").is_ok());
    auto const directory = serve();

    // The manifest declares /export/ghost and the program serves
    // /export/data: the root's Open finds nothing, and the provider's author,
    // who sees the console and not the epitaph, is told which path.
    auto const ghost = open_through(directory, "export/ghost", READABLE_DIRECTORY);
    EXPECT_EQ(ghost.status, Status::NotFound);
    EXPECT_EQ(fake::kernel().debug_writes(),
              std::vector<std::string>{"ioservice: the outgoing directory refused an Open of "
                                       "\"export/ghost\": NotFound\n"});
}

TEST_F(OutgoingTest, AnOpenRefusedBelowADeclaredPathIsTheConsumersAndNotSaid) {
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_file(data, "x", file_of(HELLO)).is_ok());
    auto const directory = serve();
    auto const entry = open_through(directory, "export/data", READABLE_DIRECTORY);
    ASSERT_EQ(entry.status, Status::Ok);
    ASSERT_TRUE(fake::kernel().debug_writes().empty());

    // The consumer asks for what is not there, for more than it holds and
    // for a path that is not valid: each reads its epitaph and the provider's
    // console stays quiet.
    EXPECT_EQ(open_through(entry.client, "missing", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(open_through(entry.client, "x", Flags::ProtocolFile | Flags::PermWriteBytes).status,
              Status::AccessDenied);
    EXPECT_EQ(open_through(entry.client, "../x", Flags::ProtocolFile).status, Status::BadPath);
    EXPECT_TRUE(fake::kernel().debug_writes().empty());

    // The root's own refusals still are, whether it opens through the
    // outgoing directory or through svc/.
    EXPECT_EQ(open_through(directory, "svc/missing", Flags::ProtocolService).status,
              Status::NotFound);
    auto const svc = open_through(directory, "svc", READABLE_DIRECTORY);
    ASSERT_EQ(svc.status, Status::Ok);
    EXPECT_EQ(open_through(svc.client, "missing", Flags::ProtocolService).status, Status::NotFound);
    EXPECT_EQ(
        fake::kernel().debug_writes(),
        (std::vector<std::string>{
            "ioservice: the outgoing directory refused an Open of \"svc/missing\": NotFound\n",
            "ioservice: the outgoing directory refused an Open of \"missing\": NotFound\n",
        }));
}
// --- a service node in a directory capability's tree -------------------------------

/// The flags of a connect() through a namespace entry: the protocol bit, and
/// no right, since the connection's own CONNECT is what is checked.
constexpr auto CONNECT_TO_SERVICE = Flags::ProtocolService;

/// The ids of the first nodes of a fresh tree. A Tree numbers its nodes in
/// the order they are added: the outgoing root, then svc/, then whatever the
/// test adds. The tests use them to name a parent add_service() must refuse,
/// which the Outgoing does not hand out.
constexpr auto ROOT_NODE = NodeId{0};
constexpr auto SVC_NODE = NodeId{1};

TEST_F(OutgoingTest, AServiceNodeInADeclaredTreeReachesItsHandler) {
    Calls stop_calls;
    Calls late_calls;
    Calls nested_calls;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    auto const sub = outgoing().add_directory(data, "sub").unwrap();
    // Before and after serve(), and at the declared directory and below it.
    auto const stop = outgoing().add_service(data, "stop", stop_calls);
    ASSERT_TRUE(stop.is_ok());
    ASSERT_TRUE(outgoing().add_service(sub, "nested", nested_calls).is_ok());
    auto const directory = serve();
    ASSERT_TRUE(outgoing().add_service(data, "late", late_calls).is_ok());

    // Each Open reaches the handler its node was added with and no other,
    // with the Open's channel, which the handler now owns.
    auto const opened = open_through(directory, "export/data/stop", CONNECT_TO_SERVICE);
    EXPECT_EQ(opened.status, Status::Ok);
    ASSERT_EQ(stop_calls.channels.size(), 1u);
    EXPECT_EQ(stop_calls.channels[0], opened.server);
    EXPECT_TRUE(late_calls.channels.empty());
    EXPECT_TRUE(nested_calls.channels.empty());
    EXPECT_TRUE(fake::kernel().is_open(opened.server));
    EXPECT_EQ(fake::kernel().queued_messages(opened.client), 0u);

    auto const late = open_through(directory, "export/data/late", CONNECT_TO_SERVICE);
    EXPECT_EQ(late.status, Status::Ok);
    ASSERT_EQ(late_calls.channels.size(), 1u);
    EXPECT_EQ(late_calls.channels[0], late.server);

    auto const nested = open_through(directory, "export/data/sub/nested", CONNECT_TO_SERVICE);
    EXPECT_EQ(nested.status, Status::Ok);
    ASSERT_EQ(nested_calls.channels.size(), 1u);
    EXPECT_EQ(nested_calls.channels[0], nested.server);
    EXPECT_EQ(stop_calls.channels.size(), 1u);
    EXPECT_EQ(late_calls.channels.size(), 1u);

    // The node is the tree's, never svc/'s, and a connect() is not a refusal
    // the provider's console would say.
    EXPECT_EQ(open_through(directory, "svc/stop", CONNECT_TO_SERVICE).status, Status::NotFound);
    EXPECT_EQ(fake::kernel().debug_writes(),
              std::vector<std::string>{"ioservice: the outgoing directory refused an Open of "
                                       "\"svc/stop\": NotFound\n"});
}

TEST_F(OutgoingTest, AServiceNodeIsReachedThroughAConsumersEntry) {
    Calls calls;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_service(data, "stop", calls).is_ok());
    auto const directory = serve();

    // What a consumer's connect("/files/stop") writes on its entry: the
    // entry's r* holds CONNECT and TRAVERSE.
    auto const entry = open_through(directory, "export/data", READABLE_DIRECTORY);
    ASSERT_EQ(entry.status, Status::Ok);
    auto const connected = open_through(entry.client, "stop", CONNECT_TO_SERVICE);
    EXPECT_EQ(connected.status, Status::Ok);
    ASSERT_EQ(calls.channels.size(), 1u);
    EXPECT_EQ(calls.channels[0], connected.server);
    // A service takes a channel and no connection: the entry's is the only
    // one besides the root's.
    EXPECT_TRUE(fake::kernel().debug_writes().empty());
}

TEST_F(OutgoingTest, AServiceNodeNeedsAConnectionThatHoldsConnect) {
    Calls calls;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_service(data, "stop", calls).is_ok());
    auto const directory = serve();

    // An entry that can read and walk but not connect: as a use of rights
    // without CONNECT would be.
    auto const entry =
        open_through(directory, "export/data",
                     Flags::ProtocolDirectory | Flags::PermReadBytes | Flags::PermTraverse);
    ASSERT_EQ(entry.status, Status::Ok);

    // Asked for as a service, and with no protocol named at all, which a
    // service would otherwise let through the generic checks: both need
    // CONNECT.
    auto const as_service = open_through(entry.client, "stop", CONNECT_TO_SERVICE);
    EXPECT_EQ(as_service.status, Status::AccessDenied);
    EXPECT_FALSE(fake::kernel().is_open(as_service.server));
    auto const unnamed = open_through(entry.client, "stop", Flags::None);
    EXPECT_EQ(unnamed.status, Status::AccessDenied);
    EXPECT_FALSE(fake::kernel().is_open(unnamed.server));
    // Rights beyond the entry's are refused first, as for any node.
    EXPECT_EQ(
        open_through(entry.client, "stop", Flags::ProtocolService | Flags::PermWriteBytes).status,
        Status::AccessDenied);
    EXPECT_TRUE(calls.channels.empty());

    // The root's own connection holds CONNECT, and reaches it.
    EXPECT_EQ(open_through(directory, "export/data/stop", CONNECT_TO_SERVICE).status, Status::Ok);
    EXPECT_EQ(calls.channels.size(), 1u);
}

TEST_F(OutgoingTest, AServiceNodeOpenedAsADirectoryOrAFileIsRefusedAndNotConnected) {
    Calls calls;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_service(data, "stop", calls).is_ok());
    auto const directory = serve();
    auto const entry = open_through(directory, "export/data", READABLE_DIRECTORY);
    ASSERT_EQ(entry.status, Status::Ok);

    // What the root writes for a use whose joined path ends at the node.
    auto const root_open = open_through(directory, "export/data/stop", READABLE_DIRECTORY);
    EXPECT_EQ(root_open.status, Status::NotDir);
    EXPECT_FALSE(fake::kernel().is_open(root_open.server));
    // And a consumer opening it as a directory through its entry.
    auto const entry_open = open_through(entry.client, "stop", READABLE_DIRECTORY);
    EXPECT_EQ(entry_open.status, Status::NotDir);
    EXPECT_FALSE(fake::kernel().is_open(entry_open.server));
    EXPECT_EQ(open_through(entry.client, "stop", Flags::ProtocolFile | Flags::PermReadBytes).status,
              Status::NotFile);
    EXPECT_TRUE(calls.channels.empty());
}

TEST_F(OutgoingTest, AddServiceRefusesAParentThatIsNotInADeclaredTree) {
    Calls calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, calls), Status::Ok);
    auto const data = outgoing().add_directory("/export/data").unwrap();
    auto const other = outgoing().add_directory("/other").unwrap();
    // The nodes added, in order: root 0, svc/ 1, the protocol's 2, export/ 3,
    // data 4, other 5. A parent the program can only get by guessing.
    auto const protocol_node = NodeId{2};
    auto const on_the_way = NodeId{std::to_underlying(data) - 1};
    ASSERT_EQ(std::to_underlying(on_the_way), 3u);

    // The outgoing root and svc/ are the protocols' places, the directory on
    // the way to a declared path belongs to no capability, a protocol's own
    // node is no place for a child, and an id of no node names nothing.
    for (auto const parent: {ROOT_NODE, SVC_NODE, protocol_node, on_the_way, NodeId{0xFFFF}}) {
        EXPECT_EQ(refusal(outgoing().add_service(parent, "x", calls)), Status::InvArgs)
            << std::to_underlying(parent);
    }

    // Nothing was made by any of them, nor a handler taken.
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "x", CONNECT_TO_SERVICE).status, Status::NotFound);
    EXPECT_EQ(open_through(directory, "svc/x", CONNECT_TO_SERVICE).status, Status::NotFound);
    EXPECT_EQ(open_through(directory, "export/x", CONNECT_TO_SERVICE).status, Status::NotFound);
    EXPECT_TRUE(calls.channels.empty());

    // Every declared directory is a place, whichever it is.
    EXPECT_TRUE(outgoing().add_service(data, "x", calls).is_ok());
    EXPECT_TRUE(outgoing().add_service(other, "x", calls).is_ok());
}

TEST_F(OutgoingTest, AddServiceRefusesAsTheTreeDoesAndLeavesNothingBehind) {
    Calls calls;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_directory(data, "sub").is_ok());
    auto const file = outgoing().add_file(data, "file", file_of(HELLO)).unwrap();
    ASSERT_TRUE(outgoing().add_service(data, "stop", calls).is_ok());

    // An invalid name; a name a file, a directory or a service has; a
    // parent that is a file.
    EXPECT_EQ(refusal(outgoing().add_service(data, "", calls)), Status::InvArgs);
    EXPECT_EQ(refusal(outgoing().add_service(data, ".", calls)), Status::InvArgs);
    EXPECT_EQ(refusal(outgoing().add_service(data, "..", calls)), Status::InvArgs);
    EXPECT_EQ(refusal(outgoing().add_service(data, "a/b", calls)), Status::InvArgs);
    EXPECT_EQ(refusal(outgoing().add_service(data, "file", calls)), Status::AlreadyExists);
    EXPECT_EQ(refusal(outgoing().add_service(data, "sub", calls)), Status::AlreadyExists);
    EXPECT_EQ(refusal(outgoing().add_service(data, "stop", calls)), Status::AlreadyExists);
    EXPECT_EQ(refusal(outgoing().add_service(file, "child", calls)), Status::NotDir);

    // None of them took a handler: the table still has room for the other
    // MAX_PROTOCOLS - 1, and then none.
    for (auto index = usize{1}; index < ioservice::Outgoing::MAX_PROTOCOLS; ++index) {
        EXPECT_TRUE(outgoing().add_service(data, keep("s" + std::to_string(index)), calls).is_ok())
            << index;
    }
    EXPECT_EQ(refusal(outgoing().add_service(data, "one-too-many", calls)), Status::NoResource);
}

TEST_F(OutgoingTest, AddServiceRefusedForTheNodesLeavesTheTreeAsItWas) {
    Calls calls;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    // export/ and data are two of the nodes for directories; the rest go to
    // files, so that the next node of any kind is refused.
    for (auto index = usize{0}; index < NODES_FOR_DIRECTORIES - 2; ++index) {
        ASSERT_TRUE(
            outgoing().add_file(data, keep("f" + std::to_string(index)), file_of(HELLO)).is_ok());
    }
    EXPECT_EQ(refusal(outgoing().add_service(data, "stop", calls)), Status::NoResource);

    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "export/data/stop", CONNECT_TO_SERVICE).status,
              Status::NotFound);
    EXPECT_TRUE(calls.channels.empty());
}

TEST_F(OutgoingTest, EightProtocolsLeaveNoRoomForAServiceNode) {
    Calls calls;
    for (auto index = usize{0}; index < ioservice::Outgoing::MAX_PROTOCOLS; ++index) {
        ASSERT_EQ(outgoing().add_protocol(keep("p" + std::to_string(index)), calls), Status::Ok);
    }
    auto const data = outgoing().add_directory("/export/data").unwrap();
    EXPECT_EQ(refusal(outgoing().add_service(data, "stop", calls)), Status::NoResource);

    // Refused before the node was added: the name is still free, which a node
    // left behind would have answered AlreadyExists, and an Open of it
    // reaches nothing.
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "export/data/stop", CONNECT_TO_SERVICE).status,
              Status::NotFound);
    EXPECT_TRUE(outgoing().add_file(data, "stop", file_of(HELLO)).is_ok());
    EXPECT_TRUE(calls.channels.empty());
}

TEST_F(OutgoingTest, EightServiceNodesLeaveNoRoomForAProtocol) {
    Calls calls;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    for (auto index = usize{0}; index < ioservice::Outgoing::MAX_PROTOCOLS; ++index) {
        ASSERT_TRUE(outgoing().add_service(data, keep("s" + std::to_string(index)), calls).is_ok());
    }
    EXPECT_EQ(outgoing().add_protocol(ECHO, calls), Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_service(data, "stop", calls)), Status::NoResource);

    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "svc/ours.test.Echo", CONNECT_TO_SERVICE).status,
              Status::NotFound);
}

TEST_F(OutgoingTest, ProtocolsAndServiceNodesTakeTheirHandlersFromOneTableInOrder) {
    // Eight handlers, each told apart, alternately added as a protocol and as a
    // service node: every Open reaches its own and no other, so the value a
    // node was added with indexes the one table whichever call made it.
    constexpr auto COUNT = ioservice::Outgoing::MAX_PROTOCOLS;
    std::array<Calls, COUNT> handlers;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    auto paths = std::vector<std::string>{};
    for (auto index = usize{0}; index < COUNT; ++index) {
        auto const name = keep("n" + std::to_string(index));
        if (index % 2 == 0) {
            ASSERT_EQ(outgoing().add_protocol(name, handlers[index]), Status::Ok);
            paths.push_back("svc/n" + std::to_string(index));
        } else {
            ASSERT_TRUE(outgoing().add_service(data, name, handlers[index]).is_ok());
            paths.push_back("export/data/n" + std::to_string(index));
        }
    }
    auto const directory = serve();

    for (auto index = usize{0}; index < COUNT; ++index) {
        auto const opened = open_through(directory, paths[index], CONNECT_TO_SERVICE);
        ASSERT_EQ(opened.status, Status::Ok) << paths[index];
        for (auto other = usize{0}; other < COUNT; ++other) {
            if (other <= index) {
                ASSERT_EQ(handlers[other].channels.size(), 1u) << index << " " << other;
            } else {
                ASSERT_TRUE(handlers[other].channels.empty()) << index << " " << other;
            }
        }
        EXPECT_EQ(handlers[index].channels[0], opened.server) << paths[index];
    }
}
// --- a Subtree node at a declared path ---------------------------------------------

TEST_F(OutgoingTest, AddSubtreeMakesTheNodeAndEveryDirectoryOnTheWay) {
    SubtreeCalls handler;
    auto const node = outgoing().add_subtree("/export/data", handler);
    ASSERT_TRUE(node.is_ok());
    auto const directory = serve();

    // export/ is a directory, as for any declared path; data is the node, and
    // an Open of it is the handler's.
    EXPECT_EQ(open_through(directory, "export", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_TRUE(handler.opens.empty());
    EXPECT_EQ(open_through(directory, "export/other", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(open_through(directory, "data", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_TRUE(handler.opens.empty());
    EXPECT_EQ(open_through(directory, "export/data", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(handler.opens.size(), 1u);
}

TEST_F(OutgoingTest, AddSubtreeWorksBeforeAndAfterServe) {
    SubtreeCalls before;
    SubtreeCalls after;
    ASSERT_TRUE(outgoing().add_subtree("/before", before).is_ok());
    auto const directory = serve();
    ASSERT_TRUE(outgoing().add_subtree("/after", after).is_ok());
    EXPECT_EQ(open_through(directory, "before", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "after/x", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(before.opens.size(), 1u);
    EXPECT_EQ(after.opens.size(), 1u);
}

TEST_F(OutgoingTest, AnOpenThatWalksToTheNodeReachesItsHandlerWithTheRestFlagsAndChannel) {
    SubtreeCalls handler;
    ASSERT_TRUE(outgoing().add_subtree("/export/data", handler).is_ok());
    auto const directory = serve();

    // The node itself, one segment after it, and several: what the root
    // writes for a use with no subdir and with one.
    auto const itself = open_through(directory, "export/data", READABLE_DIRECTORY);
    auto const one = open_through(directory, "export/data/sub", READABLE_DIRECTORY);
    auto const several = open_through(directory, "export/data/a/b/c.txt", READABLE_DIRECTORY);
    EXPECT_EQ(itself.status, Status::Ok);
    EXPECT_EQ(one.status, Status::Ok);
    EXPECT_EQ(several.status, Status::Ok);

    ASSERT_EQ(handler.opens.size(), 3u);
    EXPECT_EQ(handler.opens[0].rest, ".");
    EXPECT_EQ(handler.opens[0].flags, READABLE_DIRECTORY);
    EXPECT_EQ(handler.opens[0].channel, itself.server);
    EXPECT_EQ(handler.opens[1].rest, "sub");
    EXPECT_EQ(handler.opens[1].channel, one.server);
    EXPECT_EQ(handler.opens[2].rest, "a/b/c.txt");
    EXPECT_EQ(handler.opens[2].channel, several.server);

    // The handler owns each channel: nothing was written on it or closed. No
    // connection was made for it, and the provider's console is quiet, since
    // the Open was not refused.
    for (auto const &opened: {itself, one, several}) {
        EXPECT_TRUE(fake::kernel().is_open(opened.server));
        EXPECT_EQ(fake::kernel().queued_messages(opened.client), 0u);
    }
    EXPECT_TRUE(fake::kernel().debug_writes().empty());
    EXPECT_TRUE(fake::kernel().is_open(directory));
}

TEST_F(OutgoingTest, TheHandlerGetsTheFlagsAsTheConsumerAskedForThem) {
    SubtreeCalls handler;
    ASSERT_TRUE(outgoing().add_subtree("/export/data", handler).is_ok());
    auto const directory = serve();

    // None of them is judged for what the node is: a protocol bit that does
    // not fit a directory, the file flags a directory refuses.
    constexpr auto ALL = std::array{
        Flags::ProtocolDirectory | Flags::PermReadBytes | Flags::PermEnumerate,
        Flags::ProtocolFile | Flags::PermReadBytes,
        Flags::ProtocolService,
        Flags::ProtocolDirectory | Flags::FileAppend | Flags::FileTruncate,
        Flags::None,
    };
    for (auto const flags: ALL) {
        EXPECT_EQ(open_through(directory, "export/data/x", flags).status, Status::Ok);
    }
    ASSERT_EQ(handler.opens.size(), ALL.size());
    for (auto index = usize{0}; index < ALL.size(); ++index) {
        EXPECT_EQ(handler.opens[index].flags, ALL[index]) << index;
    }
}

TEST_F(OutgoingTest, AnOpenIsCheckedOnTheConnectionItCameOnBeforeTheHandlerIsAsked) {
    SubtreeCalls handler;
    ASSERT_TRUE(outgoing().add_subtree("/export/data", handler).is_ok());
    auto const directory = serve();

    // An entry on the directory on the way to the node, held to r*: what a
    // consumer's namespace entry is. The root's own connection holds every
    // right, so the connection that bites is this one.
    auto const entry = open_through(directory, "export", READABLE_DIRECTORY);
    ASSERT_EQ(entry.status, Status::Ok);

    // Within what it holds, the Open goes through to the handler, whole.
    EXPECT_EQ(open_through(entry.client, "data/sub", READABLE_DIRECTORY).status, Status::Ok);
    ASSERT_EQ(handler.opens.size(), 1u);
    EXPECT_EQ(handler.opens[0].rest, "sub");
    EXPECT_EQ(handler.opens[0].flags, READABLE_DIRECTORY);

    // Beyond it, the Open is refused with the epitaph and never reaches it.
    EXPECT_EQ(open_through(entry.client, "data/sub", Flags::PermWriteBytes).status,
              Status::AccessDenied);
    EXPECT_EQ(open_through(entry.client, "data", Flags::PermModifyDirectory).status,
              Status::AccessDenied);
    EXPECT_EQ(open_through(entry.client, "data/sub", Flags::PermExecute).status,
              Status::AccessDenied);
    // A path that is not valid, wherever the node is in it.
    EXPECT_EQ(open_through(entry.client, "data/../x", READABLE_DIRECTORY).status, Status::BadPath);
    EXPECT_EQ(open_through(entry.client, "data//x", READABLE_DIRECTORY).status, Status::BadPath);
    EXPECT_EQ(open_through(entry.client, "/data/x", READABLE_DIRECTORY).status, Status::BadPath);
    // Creating, and what the design cut.
    EXPECT_EQ(
        open_through(entry.client, "data/new", Flags::FlagMaybeCreate | Flags::ProtocolFile).status,
        Status::AccessDenied);
    // The root's own connection holds MODIFY_DIRECTORY, so for it creating is
    // not denied but not supported: still not the handler's to be asked.
    EXPECT_EQ(open_through(directory, "export/data/new",
                           Flags::FlagMustCreate | Flags::ProtocolFile | Flags::PermReadBytes)
                  .status,
              Status::NotSupported);
    EXPECT_EQ(open_through(entry.client, "data", Flags::ProtocolNode).status, Status::NotSupported);
    // A name that is not on the way to the node.
    EXPECT_EQ(open_through(entry.client, "nope/x", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(handler.opens.size(), 1u);

    // An entry without CONNECT cannot ask for a service through the node.
    auto const no_connect = open_through(
        directory, "export", Flags::ProtocolDirectory | Flags::PermReadBytes | Flags::PermTraverse);
    ASSERT_EQ(no_connect.status, Status::Ok);
    EXPECT_EQ(open_through(no_connect.client, "data/stop", Flags::ProtocolService).status,
              Status::AccessDenied);
    // Nor go below it without TRAVERSE.
    auto const no_traverse =
        open_through(directory, "export", Flags::ProtocolDirectory | Flags::PermReadBytes);
    ASSERT_EQ(no_traverse.status, Status::Ok);
    EXPECT_EQ(open_through(no_traverse.client, "data/x", Flags::PermReadBytes).status,
              Status::AccessDenied);
    EXPECT_EQ(handler.opens.size(), 1u);
}

TEST_F(OutgoingTest, TwoSubtreeNodesEachReachTheirOwnHandler) {
    SubtreeCalls first;
    SubtreeCalls second;
    // Sharing export/, as two siblings of any kind do.
    ASSERT_TRUE(outgoing().add_subtree("/export/a", first).is_ok());
    ASSERT_TRUE(outgoing().add_subtree("/export/b", second).is_ok());
    auto const directory = serve();

    EXPECT_EQ(open_through(directory, "export/b/two", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "export/a/one", READABLE_DIRECTORY).status, Status::Ok);
    ASSERT_EQ(first.opens.size(), 1u);
    ASSERT_EQ(second.opens.size(), 1u);
    EXPECT_EQ(first.opens[0].rest, "one");
    EXPECT_EQ(second.opens[0].rest, "two");
}

TEST_F(OutgoingTest, ASubtreeNodeAndADirectoryServedBesideEachOtherDoNotInterfere) {
    SubtreeCalls handler;
    Calls echo_calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, echo_calls), Status::Ok);
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_file(data, "x", file_of(HELLO)).is_ok());
    ASSERT_TRUE(outgoing().add_subtree("/export/live", handler).is_ok());
    auto const directory = serve();

    // The static tree is served by the core, the node by the handler, and the
    // protocol by its own, each by its path.
    auto const x =
        open_through(directory, "export/data/x", Flags::ProtocolFile | Flags::PermReadBytes);
    ASSERT_EQ(x.status, Status::Ok);
    EXPECT_EQ(read_all(x.client, 100), std::string{HELLO});
    EXPECT_TRUE(handler.opens.empty());

    EXPECT_EQ(open_through(directory, "export/live/x", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(handler.opens.size(), 1u);
    EXPECT_TRUE(echo_calls.channels.empty());

    EXPECT_EQ(open_through(directory, "svc/ours.test.Echo", Flags::ProtocolService).status,
              Status::Ok);
    EXPECT_EQ(echo_calls.channels.size(), 1u);
    EXPECT_EQ(handler.opens.size(), 1u);
}

TEST_F(OutgoingTest, ASubtreeNodeIsListedAsADirectoryNextToItsSiblings) {
    SubtreeCalls handler;
    ASSERT_TRUE(outgoing().add_subtree("/export/live", handler).is_ok());
    auto const data = outgoing().add_directory("/export/data").unwrap();
    ASSERT_TRUE(outgoing().add_file(data, "x", file_of(HELLO)).is_ok());
    auto const directory = serve();

    auto const export_dir = open_through(directory, "export", READABLE_DIRECTORY);
    ASSERT_EQ(export_dir.status, Status::Ok);
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const encoded = prot::io::encode_read_dirents(
        bytes, prot::io::ReadDirentsRequest{.txid = 1, .max_bytes = 512});
    ASSERT_TRUE(encoded.is_ok());
    EXPECT_EQ(ours::channel_write(export_dir.client, ours::ChannelWriteOptions::None,
                                  encoded.unwrap().bytes.data(),
                                  static_cast<u32>(encoded.unwrap().bytes.size()), nullptr, 0),
              Status::Ok);
    run();
    std::array<u8, wire::MAX_MESSAGE_BYTES> reply{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    ASSERT_EQ(ours::channel_read(export_dir.client, ours::ChannelReadOptions::None, reply.data(),
                                 nullptr, static_cast<u32>(reply.size()), 0, &num_bytes,
                                 &num_handles),
              Status::Ok);
    auto const decoded = prot::io::decode_read_dirents_reply(
        wire::Message{.bytes = std::span{reply}.first(num_bytes), .handles = {}}, 1);
    ASSERT_TRUE(decoded.is_ok());
    ASSERT_EQ(decoded.unwrap().status, Status::Ok);
    auto names = std::vector<std::string>{};
    auto types = std::vector<prot::io::DirentType>{};
    auto reader = prot::io::DirentReader(decoded.unwrap().dirents);
    for (auto next = reader.next(); next.is_ok() && next->is_some(); next = reader.next()) {
        names.emplace_back(next.unwrap().value().name);
        types.push_back(next.unwrap().value().type);
    }
    EXPECT_EQ(names, (std::vector<std::string>{".", "data", "live"}));
    EXPECT_EQ(types, (std::vector<prot::io::DirentType>{prot::io::DirentType::Directory,
                                                        prot::io::DirentType::Directory,
                                                        prot::io::DirentType::Directory}));
}

TEST_F(OutgoingTest, AddSubtreeRefusesAPathThatIsNotValid) {
    SubtreeCalls handler;
    // The path rules of add_directory(path), which the manifest compiler
    // holds a declared path to.
    for (auto const path:
         {"", "/", "export/data", "/export/", "/export//data", "/export/./data", "/export/../data",
          "/export/data/", "/.", "/svc", "/svc/x", "/svc/x/y"}) {
        EXPECT_EQ(refusal(outgoing().add_subtree(path, handler)), Status::InvArgs) << path;
    }
    // A segment too long to be a name.
    EXPECT_EQ(refusal(outgoing().add_subtree(keep("/" + std::string(256, 'n')), handler)),
              Status::InvArgs);
    // By segment: svcx is a name of its own.
    EXPECT_TRUE(outgoing().add_subtree("/svcx", handler).is_ok());
    EXPECT_TRUE(outgoing().add_subtree("/export/svc", handler).is_ok());
}

TEST_F(OutgoingTest, AddSubtreeRefusesAPathThatOverlapsADeclaredOne) {
    SubtreeCalls handler;
    ASSERT_TRUE(outgoing().add_subtree("/export/data", handler).is_ok());
    ASSERT_TRUE(outgoing().add_directory("/a/b").is_ok());

    // Equal to one added, above it, and below it, at any depth, whether the
    // path is a Subtree node or a directory, and whichever the declared one is.
    for (auto const path: {"/export/data", "/export", "/export/data/x", "/export/data/x/y", "/a/b",
                           "/a", "/a/b/c"}) {
        EXPECT_EQ(refusal(outgoing().add_subtree(path, handler)), Status::AlreadyExists) << path;
        EXPECT_EQ(refusal(outgoing().add_directory(path)), Status::AlreadyExists) << path;
    }

    // By segments: a path that only shares leading bytes is a different one,
    // and a sibling is.
    for (auto const path:
         {"/export/data2", "/export/dat", "/export/other", "/export2/data", "/a/c", "/data"}) {
        EXPECT_TRUE(outgoing().add_subtree(path, handler).is_ok()) << path;
    }

    // Nothing was made below the node by the refusals, and the one node was
    // not replaced: an Open still reaches the first handler.
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "a/b/c", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(open_through(directory, "export/data/x", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(handler.opens.size(), 1u);
}

TEST_F(OutgoingTest, AddSubtreeRefusedLeavesNothingBehind) {
    SubtreeCalls handler;
    // One node left. /x/y needs two: x/ must not be made and then y refused.
    for (auto index = usize{0}; index < NODES_FOR_DIRECTORIES - 1; ++index) {
        ASSERT_TRUE(outgoing().add_directory(keep("/d" + std::to_string(index))).is_ok());
    }
    EXPECT_EQ(refusal(outgoing().add_subtree("/x/y", handler)), Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_subtree("/x/y/z", handler)), Status::NoResource);

    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "x", READABLE_DIRECTORY).status, Status::NotFound);

    // No handler was taken either: the table still holds all eight, and the
    // one node left takes one.
    EXPECT_TRUE(outgoing().add_subtree("/x", handler).is_ok());
    EXPECT_EQ(open_through(directory, "x/q", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(refusal(outgoing().add_subtree("/y", handler)), Status::NoResource);
    EXPECT_EQ(handler.opens.size(), 1u);
}

TEST_F(OutgoingTest, AddSubtreeRefusedForTheTableLeavesNoDirectoryBehind) {
    SubtreeCalls handler;
    Calls calls;
    for (auto index = usize{0}; index < DECIDED_HANDLERS; ++index) {
        ASSERT_EQ(outgoing().add_protocol(keep("p" + std::to_string(index)), calls), Status::Ok);
    }
    // The nodes are there for it, the table is not: refused before export/ is
    // made, and the name is free.
    EXPECT_EQ(refusal(outgoing().add_subtree("/export/data", handler)), Status::NoResource);
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "export", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_TRUE(outgoing().add_directory("/export/data").is_ok());
    EXPECT_TRUE(handler.opens.empty());
}

TEST_F(OutgoingTest, AnInvalidPathTakesNoHandlerSlot) {
    SubtreeCalls handler;
    for (auto index = usize{0}; index < 4 * DECIDED_HANDLERS; ++index) {
        EXPECT_EQ(refusal(outgoing().add_subtree("/svc/x", handler)), Status::InvArgs);
        EXPECT_EQ(refusal(outgoing().add_subtree("relative", handler)), Status::InvArgs);
    }
    // Every slot is still free, and there are no more than eight.
    for (auto index = usize{0}; index < DECIDED_HANDLERS; ++index) {
        EXPECT_TRUE(outgoing().add_subtree(keep("/s" + std::to_string(index)), handler).is_ok())
            << index;
    }
    EXPECT_EQ(refusal(outgoing().add_subtree("/one-too-many", handler)), Status::NoResource);
}

TEST_F(OutgoingTest, NothingIsAddedBelowASubtreeNode) {
    SubtreeCalls handler;
    Calls calls;
    auto const node = outgoing().add_subtree("/export/data", handler).unwrap();
    EXPECT_EQ(refusal(outgoing().add_directory(node, "x")), Status::NotDir);
    EXPECT_EQ(refusal(outgoing().add_file(node, "x", file_of(HELLO))), Status::NotDir);
    EXPECT_EQ(refusal(outgoing().add_service(node, "x", calls)), Status::NotDir);

    // The refused add_service() took no handler: the other seven fit, and the
    // table is then full.
    for (auto index = usize{1}; index < DECIDED_HANDLERS; ++index) {
        EXPECT_TRUE(outgoing().add_subtree(keep("/s" + std::to_string(index)), handler).is_ok())
            << index;
    }
    EXPECT_EQ(refusal(outgoing().add_subtree("/one-too-many", handler)), Status::NoResource);
}

TEST_F(OutgoingTest, EightSubtreesLeaveNoRoomForAProtocolOrAServiceNode) {
    SubtreeCalls handler;
    Calls calls;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    for (auto index = usize{0}; index < DECIDED_HANDLERS; ++index) {
        ASSERT_TRUE(outgoing().add_subtree(keep("/s" + std::to_string(index)), handler).is_ok());
    }
    EXPECT_EQ(outgoing().add_protocol(ECHO, calls), Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_service(data, "stop", calls)), Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_subtree("/one-too-many", handler)), Status::NoResource);

    // Refused before a node was added: the names are free.
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "svc/ours.test.Echo", Flags::ProtocolService).status,
              Status::NotFound);
    EXPECT_EQ(open_through(directory, "export/data/stop", Flags::ProtocolService).status,
              Status::NotFound);
    EXPECT_EQ(open_through(directory, "one-too-many", READABLE_DIRECTORY).status, Status::NotFound);
}

TEST_F(OutgoingTest, EightProtocolsLeaveNoRoomForASubtreeNode) {
    SubtreeCalls handler;
    Calls calls;
    for (auto index = usize{0}; index < DECIDED_HANDLERS; ++index) {
        ASSERT_EQ(outgoing().add_protocol(keep("p" + std::to_string(index)), calls), Status::Ok);
    }
    EXPECT_EQ(refusal(outgoing().add_subtree("/export/data", handler)), Status::NoResource);
}

TEST_F(OutgoingTest, EightServiceNodesLeaveNoRoomForASubtreeNode) {
    SubtreeCalls handler;
    Calls calls;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    for (auto index = usize{0}; index < DECIDED_HANDLERS; ++index) {
        ASSERT_TRUE(outgoing().add_service(data, keep("s" + std::to_string(index)), calls).is_ok());
    }
    EXPECT_EQ(refusal(outgoing().add_subtree("/other", handler)), Status::NoResource);
}

/// What one of the table's eight places is made by.
enum class Kind : u8 {
    Protocol,
    Service,
    Subtree,
    Handler,
};

/// A table filled in the order |kinds| gives, one place each.
class MixedTable: public OutgoingTest {
  protected:
    /// Fills the table in |kinds|' order, then checks that every kind of
    /// registration is refused with NoResource and that every Open reaches the
    /// handler of the call that made its node.
    auto fill_and_check(std::array<Kind, DECIDED_HANDLERS> const &kinds) -> void {
        constexpr auto COUNT = DECIDED_HANDLERS;
        std::array<Calls, COUNT> connect_handlers;
        std::array<SubtreeCalls, COUNT> subtree_handlers;
        std::array<NodeCalls, COUNT> node_handlers;
        auto const data = outgoing().add_directory("/export/data").unwrap();
        auto paths = std::vector<std::string>{};
        for (auto index = usize{0}; index < COUNT; ++index) {
            auto const name = keep("n" + std::to_string(index));
            switch (kinds[index]) {
                case Kind::Protocol:
                    ASSERT_EQ(outgoing().add_protocol(name, connect_handlers[index]), Status::Ok)
                        << index;
                    paths.push_back("svc/n" + std::to_string(index));
                    break;
                case Kind::Service:
                    ASSERT_TRUE(outgoing().add_service(data, name, connect_handlers[index]).is_ok())
                        << index;
                    paths.push_back("export/data/n" + std::to_string(index));
                    break;
                case Kind::Subtree:
                    ASSERT_TRUE(outgoing()
                                    .add_subtree(keep("/m" + std::to_string(index)),
                                                 subtree_handlers[index])
                                    .is_ok())
                        << index;
                    paths.push_back("m" + std::to_string(index) + "/rest");
                    break;
                case Kind::Handler:
                    ASSERT_TRUE(
                        outgoing()
                            .add_handler(keep("/h" + std::to_string(index)), node_handlers[index])
                            .is_ok())
                        << index;
                    paths.push_back("h" + std::to_string(index) + "/rest");
                    break;
            }
        }

        // The ninth, of each kind, whichever the eight were.
        Calls extra;
        SubtreeCalls extra_subtree;
        NodeCalls extra_node;
        EXPECT_EQ(outgoing().add_protocol("extra", extra), Status::NoResource);
        EXPECT_EQ(refusal(outgoing().add_service(data, "extra", extra)), Status::NoResource);
        EXPECT_EQ(refusal(outgoing().add_subtree("/extra", extra_subtree)), Status::NoResource);
        EXPECT_EQ(refusal(outgoing().add_handler("/extra", extra_node)), Status::NoResource);

        auto const directory = serve();
        for (auto index = usize{0}; index < COUNT; ++index) {
            auto const flags = (kinds[index] == Kind::Subtree || kinds[index] == Kind::Handler)
                                   ? READABLE_DIRECTORY
                                   : Flags::ProtocolService;
            auto const opened = open_through(directory, paths[index], flags);
            ASSERT_EQ(opened.status, Status::Ok) << paths[index];
            // Each Open reaches the handler of its own place and no other.
            for (auto other = usize{0}; other < COUNT; ++other) {
                auto const wanted = (other <= index) ? 1u : 0u;
                if (kinds[other] == Kind::Subtree) {
                    EXPECT_EQ(subtree_handlers[other].opens.size(), wanted)
                        << index << " " << other;
                } else if (kinds[other] == Kind::Handler) {
                    EXPECT_EQ(node_handlers[other].opens.size(), wanted) << index << " " << other;
                } else {
                    EXPECT_EQ(connect_handlers[other].channels.size(), wanted)
                        << index << " " << other;
                }
            }
            if (kinds[index] == Kind::Subtree) {
                EXPECT_EQ(subtree_handlers[index].opens[0].channel, opened.server);
                EXPECT_EQ(subtree_handlers[index].opens[0].rest, "rest");
            } else if (kinds[index] == Kind::Handler) {
                // The handler is asked to open what is left of the path, and
                // the Open's channel is now a connection of the server's: the
                // handler never sees it.
                EXPECT_EQ(node_handlers[index].opens[0].path, "rest");
                EXPECT_TRUE(node_handlers[index].opens[0].directory.is_none());
                EXPECT_TRUE(fake::kernel().is_open(opened.server));
            } else {
                EXPECT_EQ(connect_handlers[index].channels[0], opened.server);
            }
        }
        EXPECT_TRUE(extra.channels.empty());
        EXPECT_TRUE(extra_subtree.opens.empty());
        EXPECT_TRUE(extra_node.opens.empty());
    }
};

TEST_F(MixedTable, ProtocolServiceSubtreeInRotation) {
    fill_and_check({Kind::Protocol, Kind::Service, Kind::Subtree, Kind::Protocol, Kind::Service,
                    Kind::Subtree, Kind::Protocol, Kind::Service});
}

TEST_F(MixedTable, SubtreeFirstThenTheOthersReversed) {
    fill_and_check({Kind::Subtree, Kind::Subtree, Kind::Service, Kind::Protocol, Kind::Subtree,
                    Kind::Protocol, Kind::Service, Kind::Subtree});
}

TEST_F(MixedTable, ServiceNodesAndSubtreesOnly) {
    fill_and_check({Kind::Service, Kind::Subtree, Kind::Service, Kind::Subtree, Kind::Service,
                    Kind::Subtree, Kind::Service, Kind::Subtree});
}

TEST_F(MixedTable, TheLastPlaceIsASubtree) {
    fill_and_check({Kind::Protocol, Kind::Protocol, Kind::Protocol, Kind::Service, Kind::Service,
                    Kind::Service, Kind::Protocol, Kind::Subtree});
}

TEST_F(MixedTable, AllFourKindsInRotation) {
    fill_and_check({Kind::Handler, Kind::Protocol, Kind::Service, Kind::Subtree, Kind::Handler,
                    Kind::Protocol, Kind::Service, Kind::Subtree});
}

TEST_F(MixedTable, HandlersFirstThenTheOthers) {
    fill_and_check({Kind::Handler, Kind::Handler, Kind::Handler, Kind::Protocol, Kind::Subtree,
                    Kind::Service, Kind::Handler, Kind::Protocol});
}

TEST_F(MixedTable, TheLastPlaceIsAHandler) {
    fill_and_check({Kind::Protocol, Kind::Service, Kind::Subtree, Kind::Protocol, Kind::Service,
                    Kind::Subtree, Kind::Protocol, Kind::Handler});
}

TEST_F(MixedTable, HandlersAndSubtreesOnly) {
    fill_and_check({Kind::Handler, Kind::Subtree, Kind::Handler, Kind::Subtree, Kind::Handler,
                    Kind::Subtree, Kind::Handler, Kind::Subtree});
}

TEST_F(MixedTable, EightHandlers) {
    fill_and_check({Kind::Handler, Kind::Handler, Kind::Handler, Kind::Handler, Kind::Handler,
                    Kind::Handler, Kind::Handler, Kind::Handler});
}

TEST_F(OutgoingTest, AHandlerThatParksAnOpenAnswersItLaterOnItsChannel) {
    SubtreeCalls handler;
    ASSERT_TRUE(outgoing().add_subtree("/export/data", handler).is_ok());
    auto const directory = serve();
    auto const parked = open_through(directory, "export/data/late", READABLE_DIRECTORY);
    ASSERT_EQ(parked.status, Status::Ok);
    ASSERT_EQ(handler.opens.size(), 1u);

    // Other Opens go by, which rewrite the request the rest was viewed in.
    EXPECT_EQ(open_through(directory, "export/data/another-longer-path", READABLE_DIRECTORY).status,
              Status::Ok);
    EXPECT_EQ(open_through(directory, "export/missing", READABLE_DIRECTORY).status,
              Status::NotFound);
    EXPECT_EQ(handler.opens[0].rest, "late");

    // The answer comes later, on the channel the handler kept.
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const message = wire::encode_epitaph(bytes, Status::NotSupported);
    ASSERT_TRUE(message.is_ok());
    EXPECT_EQ(ours::channel_write(handler.opens[0].channel, ours::ChannelWriteOptions::None,
                                  message.unwrap().bytes.data(),
                                  static_cast<u32>(message.unwrap().bytes.size()), nullptr, 0),
              Status::Ok);
    EXPECT_EQ(epitaph(parked.client), Status::NotSupported);
}

// --- a Handler node at a declared path ----------------------------------------------

/// What a file Open asks for: read, as a use of r* does.
constexpr auto READABLE_FILE = Flags::ProtocolFile | Flags::PermReadBytes;

/// Writes |message| on |channel|.
auto send(Uandle const channel, wire::Message const message) -> void {
    EXPECT_EQ(ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                                  static_cast<u32>(message.bytes.size()), message.handles.data(),
                                  static_cast<u32>(message.handles.size())),
              Status::Ok);
}

/// Writes Read(|txid|, |count|) on the file connection |file|.
auto send_read(Uandle const file, u32 const txid, u64 const count) -> void {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const encoded =
        prot::io::encode_read(bytes, prot::io::ReadRequest{.txid = txid, .count = count});
    ASSERT_TRUE(encoded.is_ok());
    send(file, encoded.unwrap());
}

/// The data of the Ok answer to Read(|txid|) waiting on |file|; none when no
/// message is, or when it is not that answer.
auto read_reply(Uandle const file, u32 const txid) -> std::optional<std::string> {
    std::array<u8, wire::MAX_MESSAGE_BYTES> reply{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (ours::channel_read(file, ours::ChannelReadOptions::None, reply.data(), nullptr,
                           static_cast<u32>(reply.size()), 0, &num_bytes,
                           &num_handles) != Status::Ok) {
        return std::nullopt;
    }
    auto const decoded = prot::io::decode_read_reply(
        wire::Message{.bytes = std::span{reply}.first(num_bytes), .handles = {}}, txid);
    if (decoded.is_err() || decoded.unwrap().status != Status::Ok) {
        return std::nullopt;
    }
    auto const data = decoded.unwrap().data;
    return std::string{data.begin(), data.end()};
}

/// What a completed Read of |text| is.
auto read_of(std::string_view const text) -> ioservice::ReadResult {
    return ustl::ok(std::span<u8 const>{reinterpret_cast<u8 const *>(text.data()), text.size()});
}

TEST_F(OutgoingTest, AddHandlerMakesTheNodeAndEveryDirectoryOnTheWay) {
    NodeCalls handler;
    ASSERT_TRUE(outgoing().add_handler("/export/data", handler).is_ok());
    auto const directory = serve();

    // export/ is a directory, as for any declared path; data is the node, and
    // an Open of it is the handler's.
    EXPECT_EQ(open_through(directory, "export", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_TRUE(handler.opens.empty());
    EXPECT_EQ(open_through(directory, "export/other", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(open_through(directory, "data", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_TRUE(handler.opens.empty());
    EXPECT_EQ(open_through(directory, "export/data", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(handler.opens.size(), 1u);
}

TEST_F(OutgoingTest, AddHandlerWorksBeforeAndAfterServe) {
    NodeCalls before;
    NodeCalls after;
    ASSERT_TRUE(outgoing().add_handler("/before", before).is_ok());
    auto const directory = serve();
    ASSERT_TRUE(outgoing().add_handler("/after", after).is_ok());
    EXPECT_EQ(open_through(directory, "before", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "after/x", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(before.opens.size(), 1u);
    EXPECT_EQ(after.opens.size(), 1u);
}

TEST_F(OutgoingTest, AnOpenThatWalksToTheNodeAsksTheHandlerToOpenTheRestWithTheFlagsAsAsked) {
    NodeCalls handler;
    ASSERT_TRUE(outgoing().add_handler("/export/data", handler).is_ok());
    auto const directory = serve();

    // The node itself, one segment after it, and several: what the root writes
    // for a use with no subdir and with one.
    EXPECT_EQ(open_through(directory, "export/data", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "export/data/sub", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "export/data/a/b/c", READABLE_DIRECTORY).status, Status::Ok);

    ASSERT_EQ(handler.opens.size(), 3u);
    EXPECT_EQ(handler.opens[0].path, ".");
    EXPECT_EQ(handler.opens[1].path, "sub");
    EXPECT_EQ(handler.opens[2].path, "a/b/c");
    for (auto const &open: handler.opens) {
        // The Open walked to the node: no directory of the handler's was
        // involved. The flags are the consumer's own.
        EXPECT_TRUE(open.directory.is_none());
        EXPECT_EQ(open.flags, READABLE_DIRECTORY);
    }
    // The connection the server made for each holds exactly the rights asked,
    // and the provider's console is quiet.
    EXPECT_TRUE(fake::kernel().debug_writes().empty());
}

TEST_F(OutgoingTest, AnOpenIsCheckedOnTheConnectionItCameOnBeforeAHandlerNodesHandlerIsAsked) {
    NodeCalls handler;
    ASSERT_TRUE(outgoing().add_handler("/export/data", handler).is_ok());
    auto const directory = serve();
    auto const entry = open_through(directory, "export", READABLE_DIRECTORY);
    ASSERT_EQ(entry.status, Status::Ok);

    // Beyond the r* of the entry, a bad path, or a creation flag: refused with
    // the epitaph, and the handler is never asked.
    EXPECT_EQ(open_through(entry.client, "data/sub", Flags::PermWriteBytes).status,
              Status::AccessDenied);
    EXPECT_EQ(open_through(entry.client, "data/../x", READABLE_DIRECTORY).status, Status::BadPath);
    EXPECT_EQ(
        open_through(entry.client, "data/new", Flags::FlagMaybeCreate | Flags::ProtocolFile).status,
        Status::AccessDenied);
    EXPECT_TRUE(handler.opens.empty());
    // Within it, the handler is asked.
    EXPECT_EQ(open_through(entry.client, "data/sub", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(handler.opens.size(), 1u);
}

TEST_F(OutgoingTest, TheHandlerIsToldTheValueItWasAddedWithAndNeverTheTableIndex) {
    NodeCalls handler;
    Calls calls;
    // A protocol first, so that no handler's table index is its value by
    // accident: the three below are entries 1, 2 and 3 of the table.
    ASSERT_EQ(outgoing().add_protocol(ECHO, calls), Status::Ok);
    ASSERT_TRUE(outgoing().add_handler("/a", handler, 55).is_ok());
    ASSERT_TRUE(outgoing().add_handler("/b", handler, 66).is_ok());
    ASSERT_TRUE(outgoing().add_handler("/c", handler).is_ok());
    auto const directory = serve();

    EXPECT_EQ(open_through(directory, "b/x", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "a/x", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "c/x", READABLE_DIRECTORY).status, Status::Ok);
    ASSERT_EQ(handler.opens.size(), 3u);
    EXPECT_EQ(handler.opens[0].value, 66u);
    EXPECT_EQ(handler.opens[1].value, 55u);
    EXPECT_EQ(handler.opens[2].value, 0u);
}

TEST_F(OutgoingTest, EveryRequestOnAHandlersConnectionsReachesItWithTheValueAndTheCookie) {
    NodeCalls handler;
    ASSERT_TRUE(outgoing().add_handler("/export/data", handler, 9).is_ok());
    auto const directory = serve();
    auto const entry = open_through(directory, "export/data", READABLE_DIRECTORY);
    ASSERT_EQ(entry.status, Status::Ok);

    // An Open through the directory the handler opened is told its cookie, and
    // the whole path the client sent.
    auto const file = open_through(entry.client, "file", READABLE_FILE);
    ASSERT_EQ(file.status, Status::Ok);
    ASSERT_EQ(handler.opens.size(), 2u);
    EXPECT_EQ(handler.opens[1].value, 9u);
    ASSERT_TRUE(handler.opens[1].directory.is_some());
    EXPECT_EQ(*handler.opens[1].directory, NodeCalls::DIRECTORY_COOKIE);
    EXPECT_EQ(handler.opens[1].path, "file");

    // A Read: the file's cookie and the connection's offset.
    EXPECT_EQ(read_all(file.client, 100).value_or("<none>"), "handler");
    ASSERT_EQ(handler.reads.size(), 1u);
    EXPECT_EQ(handler.reads[0].value, 9u);
    EXPECT_EQ(handler.reads[0].cookie, NodeCalls::FILE_COOKIE);
    EXPECT_EQ(handler.reads[0].offset, 0u);
    EXPECT_EQ(handler.reads[0].count, 100u);

    // A ReadDirents and a GetBackingMemory.
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const listing = prot::io::encode_read_dirents(
        bytes, prot::io::ReadDirentsRequest{.txid = 3, .max_bytes = 256});
    ASSERT_TRUE(listing.is_ok());
    send(entry.client, listing.unwrap());
    run();
    ASSERT_EQ(handler.listings.size(), 1u);
    EXPECT_EQ(handler.listings[0].value, 9u);
    EXPECT_EQ(handler.listings[0].cookie, NodeCalls::DIRECTORY_COOKIE);
    auto const vmo = prot::io::encode_get_backing_memory(
        bytes, prot::io::GetBackingMemoryRequest{.txid = 4, .flags = prot::io::VmoFlags::Read});
    ASSERT_TRUE(vmo.is_ok());
    send(file.client, vmo.unwrap());
    run();
    ASSERT_EQ(handler.vmos.size(), 1u);
    EXPECT_EQ(handler.vmos[0].value, 9u);
    EXPECT_EQ(handler.vmos[0].cookie, NodeCalls::FILE_COOKIE);

    // A Clone of the file: the handler names the new connection.
    auto const [clone_client, clone_server] = channel();
    std::array<Uandle, 1> handles{};
    auto const cloned =
        prot::io::encode_clone(bytes, handles, prot::io::CloneRequest{.request = clone_server});
    ASSERT_TRUE(cloned.is_ok());
    send(file.client, cloned.unwrap());
    run();
    ASSERT_EQ(handler.clones.size(), 1u);
    EXPECT_EQ(handler.clones[0].value, 9u);
    EXPECT_EQ(handler.clones[0].cookie, NodeCalls::FILE_COOKIE);

    // Closing the clone ends its connection: the handler is told, once, with
    // the cookie it gave.
    auto const closed = prot::io::encode_close(bytes, prot::io::CloseRequest{.txid = 5});
    ASSERT_TRUE(closed.is_ok());
    send(clone_client, closed.unwrap());
    run();
    ASSERT_EQ(handler.releases.size(), 1u);
    EXPECT_EQ(handler.releases[0].value, 9u);
    EXPECT_EQ(handler.releases[0].cookie, NodeCalls::FILE_COOKIE + NodeCalls::CLONE_OFFSET);
}

TEST_F(OutgoingTest, ARequestTheHandlerAnswersPendingIsEndedByComplete) {
    NodeCalls handler;
    handler.pend_read = true;
    ASSERT_TRUE(outgoing().add_handler("/export/data", handler, 9).is_ok());
    auto const directory = serve();
    auto const entry = open_through(directory, "export/data", READABLE_DIRECTORY);
    auto const file = open_through(entry.client, "file", READABLE_FILE);
    ASSERT_EQ(file.status, Status::Ok);

    // Two Reads written back to back: only the first is started.
    send_read(file.client, 1, 4);
    send_read(file.client, 2, 3);
    run();
    ASSERT_EQ(handler.reads.size(), 1u);
    EXPECT_EQ(fake::kernel().queued_messages(file.client), 0u);
    auto const first = handler.reads[0].ticket;

    EXPECT_EQ(outgoing().complete(first, read_of("late")), Status::Ok);
    EXPECT_EQ(read_reply(file.client, 1).value_or("<none>"), "late");
    // A spent ticket is refused, with nothing written.
    EXPECT_EQ(outgoing().complete(first, read_of("late")), Status::NotFound);
    EXPECT_EQ(fake::kernel().queued_messages(file.client), 0u);

    // The second starts on the next turn, where the first ended.
    run();
    ASSERT_EQ(handler.reads.size(), 2u);
    EXPECT_EQ(handler.reads[1].offset, 4u);
    EXPECT_EQ(outgoing().complete(handler.reads[1].ticket, read_of("er")), Status::Ok);
    EXPECT_EQ(read_reply(file.client, 2).value_or("<none>"), "er");
}

TEST_F(OutgoingTest, AConnectionThatEndsTellsTheHandlerOnce) {
    NodeCalls handler;
    ASSERT_TRUE(outgoing().add_handler("/export/data", handler, 9).is_ok());
    auto const directory = serve();
    auto const entry = open_through(directory, "export/data", READABLE_DIRECTORY);
    ASSERT_EQ(entry.status, Status::Ok);
    EXPECT_TRUE(handler.releases.empty());

    EXPECT_EQ(ours::handle_close(entry.client), Status::Ok);
    run();
    ASSERT_EQ(handler.releases.size(), 1u);
    EXPECT_EQ(handler.releases[0].value, 9u);
    EXPECT_EQ(handler.releases[0].cookie, NodeCalls::DIRECTORY_COOKIE);
    run();
    EXPECT_EQ(handler.releases.size(), 1u);
}

TEST_F(OutgoingTest, AddHandlerRefusesAPathThatIsNotValid) {
    NodeCalls handler;
    for (auto const path:
         {"", "/", "export/data", "/export/", "/export//data", "/export/./data", "/export/../data",
          "/export/data/", "/.", "/svc", "/svc/x", "/svc/x/y"}) {
        EXPECT_EQ(refusal(outgoing().add_handler(path, handler)), Status::InvArgs) << path;
    }
    EXPECT_EQ(refusal(outgoing().add_handler(keep("/" + std::string(256, 'n')), handler)),
              Status::InvArgs);
    // By segment: svcx is a name of its own.
    EXPECT_TRUE(outgoing().add_handler("/svcx", handler).is_ok());
    EXPECT_TRUE(outgoing().add_handler("/export/svc", handler).is_ok());
}

TEST_F(OutgoingTest, AddHandlerRefusesAPathThatOverlapsADeclaredOneOfAnyKind) {
    NodeCalls handler;
    SubtreeCalls subtree;
    ASSERT_TRUE(outgoing().add_handler("/export/data", handler).is_ok());
    ASSERT_TRUE(outgoing().add_directory("/a/b").is_ok());
    ASSERT_TRUE(outgoing().add_subtree("/s/t", subtree).is_ok());

    // Equal to one added, above it and below it, whichever kind of declared
    // path it is, and whichever kind the new one is.
    for (auto const path: {"/export/data", "/export", "/export/data/x", "/a/b", "/a", "/a/b/c",
                           "/s/t", "/s", "/s/t/u"}) {
        EXPECT_EQ(refusal(outgoing().add_handler(path, handler)), Status::AlreadyExists) << path;
        EXPECT_EQ(refusal(outgoing().add_directory(path)), Status::AlreadyExists) << path;
        EXPECT_EQ(refusal(outgoing().add_subtree(path, subtree)), Status::AlreadyExists) << path;
    }
    // By segments: a path that only shares leading bytes is a different one,
    // and a sibling is. Six of them fit the six places the table has left.
    for (auto const path:
         {"/export/data2", "/export/dat", "/export/other", "/export2/data", "/a/c", "/s/u"}) {
        EXPECT_TRUE(outgoing().add_handler(path, handler).is_ok()) << path;
    }
    // None of the refusals replaced a node: an Open still reaches the first
    // handler.
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "export/data/x", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(open_through(directory, "a/b/c", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_EQ(handler.opens.size(), 1u);
}

TEST_F(OutgoingTest, AddHandlerRefusedForRoomLeavesNothingBehind) {
    NodeCalls handler;
    // One node left. /x/y needs two: x/ must not be made and then y refused.
    for (auto index = usize{0}; index < NODES_FOR_DIRECTORIES - 1; ++index) {
        ASSERT_TRUE(outgoing().add_directory(keep("/d" + std::to_string(index))).is_ok());
    }
    EXPECT_EQ(refusal(outgoing().add_handler("/x/y", handler)), Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_handler("/x/y/z", handler)), Status::NoResource);

    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "x", READABLE_DIRECTORY).status, Status::NotFound);

    // No handler was taken either: the one node left takes one.
    EXPECT_TRUE(outgoing().add_handler("/x", handler).is_ok());
    EXPECT_EQ(open_through(directory, "x/q", READABLE_DIRECTORY).status, Status::Ok);
    EXPECT_EQ(refusal(outgoing().add_handler("/y", handler)), Status::NoResource);
}

TEST_F(OutgoingTest, AddHandlerRefusedForTheTableLeavesNoDirectoryBehind) {
    NodeCalls handler;
    Calls calls;
    for (auto index = usize{0}; index < DECIDED_HANDLERS; ++index) {
        ASSERT_EQ(outgoing().add_protocol(keep("p" + std::to_string(index)), calls), Status::Ok);
    }
    // The nodes are there for it, the table is not: refused before export/ is
    // made, and the name is free.
    EXPECT_EQ(refusal(outgoing().add_handler("/export/data", handler)), Status::NoResource);
    auto const directory = serve();
    EXPECT_EQ(open_through(directory, "export", READABLE_DIRECTORY).status, Status::NotFound);
    EXPECT_TRUE(outgoing().add_directory("/export/data").is_ok());
    EXPECT_TRUE(handler.opens.empty());
}

TEST_F(OutgoingTest, AnInvalidPathTakesNoHandlerSlotFromAHandler) {
    NodeCalls handler;
    for (auto index = usize{0}; index < 4 * DECIDED_HANDLERS; ++index) {
        EXPECT_EQ(refusal(outgoing().add_handler("/svc/x", handler)), Status::InvArgs);
        EXPECT_EQ(refusal(outgoing().add_handler("relative", handler)), Status::InvArgs);
        EXPECT_EQ(refusal(outgoing().add_handler("/", handler)), Status::InvArgs);
    }
    for (auto index = usize{0}; index < DECIDED_HANDLERS; ++index) {
        EXPECT_TRUE(outgoing().add_handler(keep("/h" + std::to_string(index)), handler).is_ok())
            << index;
    }
    EXPECT_EQ(refusal(outgoing().add_handler("/one-too-many", handler)), Status::NoResource);
}

TEST_F(OutgoingTest, NothingIsAddedBelowAHandlerNode) {
    NodeCalls handler;
    Calls calls;
    auto const node = outgoing().add_handler("/export/data", handler).unwrap();
    EXPECT_EQ(refusal(outgoing().add_directory(node, "x")), Status::NotDir);
    EXPECT_EQ(refusal(outgoing().add_file(node, "x", file_of(HELLO))), Status::NotDir);
    EXPECT_EQ(refusal(outgoing().add_service(node, "x", calls)), Status::NotDir);

    // The refused add_service() took no handler: the other seven fit, and the
    // table is then full.
    for (auto index = usize{1}; index < DECIDED_HANDLERS; ++index) {
        EXPECT_TRUE(outgoing().add_handler(keep("/h" + std::to_string(index)), handler).is_ok())
            << index;
    }
    EXPECT_EQ(refusal(outgoing().add_handler("/one-too-many", handler)), Status::NoResource);
}

TEST_F(OutgoingTest, EightHandlersLeaveNoRoomForAProtocolAServiceNodeOrASubtree) {
    NodeCalls handler;
    Calls calls;
    SubtreeCalls subtree;
    auto const data = outgoing().add_directory("/export/data").unwrap();
    for (auto index = usize{0}; index < DECIDED_HANDLERS; ++index) {
        ASSERT_TRUE(outgoing().add_handler(keep("/h" + std::to_string(index)), handler).is_ok());
    }
    EXPECT_EQ(outgoing().add_protocol(ECHO, calls), Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_service(data, "stop", calls)), Status::NoResource);
    EXPECT_EQ(refusal(outgoing().add_subtree("/s", subtree)), Status::NoResource);
}

TEST_F(OutgoingTest, EightProtocolsLeaveNoRoomForAHandlerNode) {
    NodeCalls handler;
    Calls calls;
    for (auto index = usize{0}; index < DECIDED_HANDLERS; ++index) {
        ASSERT_EQ(outgoing().add_protocol(keep("p" + std::to_string(index)), calls), Status::Ok);
    }
    EXPECT_EQ(refusal(outgoing().add_handler("/export/data", handler)), Status::NoResource);
}
} // namespace
