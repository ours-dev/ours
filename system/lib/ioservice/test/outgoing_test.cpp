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
// and the connections and nodes that run out.
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
#include <ours_io/ours_io.hpp>
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
using ours_io::Flags;

constexpr auto ECHO = std::string_view{"ours.test.Echo"};
constexpr auto OTHER = std::string_view{"ours.test.Other"};

/// What a use that asks for r* comes to as an Open's flags: the rights, and
/// the protocol bit that makes a file or a service answer NotDir. The root
/// writes this, with the declared path and the joined subdir, for each
/// directory use that routes to a provider.
constexpr auto READABLE_DIRECTORY = Flags::ProtocolDirectory | ours_io::PERM_READABLE;

/// The tests use the decided sizes as numbers, not through the constants, so
/// that a constant changed by mistake fails them instead of moving them.
constexpr auto DECIDED_CONNECTIONS = usize{32};
constexpr auto DECIDED_NODES = usize{32};

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
        auto const encoded = ours_io::encode_open(
            bytes, handles, ours_io::OpenRequest{.flags = flags, .path = path, .object = object});
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
            ours_io::encode_read(bytes, ours_io::ReadRequest{.txid = 1, .count = count});
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
        auto const decoded = ours_io::decode_read_reply(
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
    while (too_long.size() <= ours_io::MAX_PATH_LENGTH) {
        too_long += "/a";
    }
    auto const long_path = keep(too_long);
    auto const long_name = keep("/" + std::string(ours_io::MAX_NAME_LENGTH + 1, 'x'));
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
        open_through(directory, "export/data", Flags::ProtocolDirectory | ours_io::PERM_WRITABLE)
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
} // namespace
