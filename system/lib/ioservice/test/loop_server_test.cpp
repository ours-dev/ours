/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// ioservice::LoopServer on a real Loop, against the fake kernel of
// fake_kernel.hpp: what serve() refuses, and what each kind of message on a
// served directory comes to -- an Open of a service node handed to the
// connect function, an Open of a name that is not there answered NotFound, a
// directory opened through it, a reply written back, a Close answered, a
// malformed message, a reply that cannot be written, a client gone, and a
// Loop that cannot arm a connection's wait again.
#include <ioservice/loop_server.hpp>

#include <array>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
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
using ours::Status;
using ours::Uandle;
using ours_io::Flags;

/// The connections the server allows at once.
constexpr auto CONNECTIONS = usize{4};

/// The value each service node is added with.
constexpr auto ECHO_SERVICE = u64{7};
constexpr auto NESTED_SERVICE = u64{9};

/// The server's delegate, which keeps what it was called with, in order.
class Calls final: public ioservice::LoopServer::Delegate {
  public:
    struct Connect {
        u64 service;
        Uandle channel;
    };

    std::vector<Connect> connects;
    std::vector<Status> write_failures;

    auto connect(u64 const service, Uandle const channel) -> void override {
        connects.push_back({.service = service, .channel = channel});
    }

    auto write_failed(Status const status) -> void override {
        write_failures.push_back(status);
    }
};

/// A channel pair: the client end first.
auto channel() -> std::array<Uandle, 2> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &client, &server), Status::Ok);
    return {client, server};
}

/// Writes |message| on |channel|.
auto write(Uandle const channel, wire::Message const message) -> void {
    EXPECT_EQ(ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                                  static_cast<u32>(message.bytes.size()), message.handles.data(),
                                  static_cast<u32>(message.handles.size())),
              Status::Ok);
}

/// Writes Open(|path|, |flags|, |object|) on |directory|.
auto open(Uandle const directory, std::string_view const path, Flags const flags,
          Uandle const object) -> void {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, 1> handles{};
    auto const encoded = ours_io::encode_open(
        bytes, handles, ours_io::OpenRequest{.flags = flags, .path = path, .object = object});
    ASSERT_TRUE(encoded.is_ok());
    write(directory, encoded.unwrap());
}

/// Writes ReadDirents(|txid|) on |directory|.
auto request_dirents(Uandle const directory, u32 const txid) -> void {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const encoded = ours_io::encode_read_dirents(
        bytes, ours_io::ReadDirentsRequest{.txid = txid, .max_bytes = 512});
    ASSERT_TRUE(encoded.is_ok());
    write(directory, encoded.unwrap());
}

/// The bytes of the one message waiting on |channel|; none when nothing is.
auto receive(Uandle const channel) -> std::optional<std::vector<u8>> {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> handles{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (ours::channel_read(channel, ours::ChannelReadOptions::None, bytes.data(), handles.data(),
                           static_cast<u32>(bytes.size()), static_cast<u32>(handles.size()),
                           &num_bytes, &num_handles) != Status::Ok) {
        return std::nullopt;
    }
    return std::vector<u8>(bytes.begin(), bytes.begin() + num_bytes);
}

/// Checks that the one message waiting on |directory| is the Ok answer to
/// ReadDirents(|txid|).
auto expect_dirents_reply(Uandle const directory, u32 const txid) -> void {
    auto const reply = receive(directory);
    ASSERT_TRUE(reply.has_value());
    auto const decoded =
        ours_io::decode_read_dirents_reply(wire::Message{.bytes = *reply, .handles = {}}, txid);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded.unwrap().status, Status::Ok);
    EXPECT_FALSE(decoded.unwrap().dirents.empty());
}

/// The status of the epitaph waiting on |channel|; InvArgs when what is
/// there is no epitaph, ShouldWait when nothing is, PeerClosed when the
/// channel was closed with nothing written.
auto epitaph(Uandle const channel) -> Status {
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

/// What the server keeps, which is too large for the stack.
struct Storage {
    std::array<ioservice::Node, 8> nodes;
    std::array<ioservice::Connection, CONNECTIONS> connections;
    std::array<ioservice::LoopServer::Binding, CONNECTIONS> bindings;
    std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
    ioservice::ReplyBuffer reply;
};

/// A fresh fake kernel, a Loop on it, and a LoopServer on the Loop, over the
/// tree
///
///   /          the directory the tests serve
///   /echo      a service, ECHO_SERVICE
///   /sub       a directory
///   /sub/nested  a service, NESTED_SERVICE
class LoopServerTest: public testing::Test {
  protected:
    LoopServerTest() : m_storage(std::make_unique<Storage>()), m_tree(m_storage->nodes) {
        fake::kernel().reset();
        auto port = Uandle::Invalid;
        EXPECT_EQ(ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok);

        m_root = m_tree.add_root().unwrap();
        m_echo = m_tree.add_service(m_root, "echo", ECHO_SERVICE).unwrap();
        auto const sub = m_tree.add_directory(m_root, "sub").unwrap();
        // The tests find /sub/nested by its path, so its id is not kept.
        auto _ = m_tree.add_service(sub, "nested", NESTED_SERVICE).unwrap();

        m_loop.emplace(port, m_slots);
        m_server.emplace(m_tree, *m_loop, syscalls(), memory(), &m_calls);
    }

    static auto syscalls() -> ioservice::Syscalls {
        return {.handle_duplicate = ours::handle_duplicate};
    }

    auto memory() -> ioservice::LoopServer::Memory {
        return {
            .connections = m_storage->connections,
            .bindings = m_storage->bindings,
            .request_bytes = m_storage->request_bytes,
            .request_handles = m_storage->request_handles,
            .reply = &m_storage->reply,
        };
    }

    auto server() -> ioservice::LoopServer & {
        return *m_server;
    }

    auto calls() -> Calls & {
        return m_calls;
    }

    auto tree() -> ioservice::Tree & {
        return m_tree;
    }

    auto event_loop() -> loop::Loop & {
        return *m_loop;
    }

    /// Dispatches everything queued, and checks that the port ended empty.
    auto run() -> void {
        EXPECT_EQ(m_loop->run(0), Status::TimeOut);
    }

    /// A channel pair, of which the server end is serving the root
    /// directory: the client end first.
    auto serve_root() -> std::array<Uandle, 2> {
        auto const ends = channel();
        EXPECT_EQ(server().serve(ends[1], m_root, ours_io::R_STAR_DIR), Status::Ok);
        return ends;
    }

    ioservice::NodeId m_root = {};
    ioservice::NodeId m_echo = {};

  private:
    std::array<loop::Slot, 8> m_slots{};
    std::unique_ptr<Storage> m_storage;
    ioservice::Tree m_tree;
    std::optional<loop::Loop> m_loop;
    std::optional<ioservice::LoopServer> m_server;
    Calls m_calls;
};

TEST_F(LoopServerTest, ServeRefusals) {
    EXPECT_EQ(server().serve(Uandle::Invalid, m_root, ours_io::R_STAR_DIR), Status::InvArgs);

    // A service is not a directory, and nothing is served.
    auto const [client, server_end] = channel();
    EXPECT_EQ(server().serve(server_end, m_echo, ours_io::R_STAR_DIR), Status::NotDir);
    EXPECT_TRUE(fake::kernel().is_open(server_end));
}

TEST_F(LoopServerTest, ServeRefusesPastItsConnections) {
    for (auto index = usize{0}; index < CONNECTIONS; ++index) {
        serve_root();
    }
    auto const [client, server_end] = channel();
    EXPECT_EQ(server().serve(server_end, m_root, ours_io::R_STAR_DIR), Status::NoResource);
    // Refused, so still the caller's.
    EXPECT_TRUE(fake::kernel().is_open(server_end));
}

TEST_F(LoopServerTest, OpenOfAServiceReachesTheConnector) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, service_end] = channel();
    open(directory, "echo", Flags::ProtocolService, service_end);
    run();

    ASSERT_EQ(calls().connects.size(), 1u);
    EXPECT_EQ(calls().connects[0].service, ECHO_SERVICE);
    EXPECT_EQ(calls().connects[0].channel, service_end);
    // The connect function owns the channel: nothing was written on it or
    // closed, and the directory's connection stays for the next Open.
    EXPECT_TRUE(fake::kernel().is_open(service_end));
    EXPECT_EQ(fake::kernel().queued_messages(client), 0u);
    EXPECT_TRUE(fake::kernel().is_open(directory_server));
}

TEST_F(LoopServerTest, OpenOfAServiceWithoutAConnectorClosesItsChannel) {
    server().set_delegate(nullptr);
    auto const [directory, directory_server] = serve_root();
    auto const [client, service_end] = channel();
    open(directory, "echo", Flags::ProtocolService, service_end);
    run();

    EXPECT_FALSE(fake::kernel().is_open(service_end));
    EXPECT_TRUE(fake::kernel().is_open(directory_server));
}

TEST_F(LoopServerTest, OpenOfAMissingNameAnswersNotFound) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, service_end] = channel();
    open(directory, "missing", Flags::ProtocolService, service_end);
    run();

    EXPECT_TRUE(calls().connects.empty());
    EXPECT_EQ(epitaph(client), Status::NotFound);
    EXPECT_FALSE(fake::kernel().is_open(service_end));
    EXPECT_TRUE(fake::kernel().is_open(directory_server));
}

TEST_F(LoopServerTest, OpenOfADirectoryMakesAConnectionOfItsOwn) {
    auto const [directory, directory_server] = serve_root();
    auto const [sub_client, sub_server] = channel();
    open(directory, "sub", Flags::ProtocolDirectory | Flags::PermConnect | Flags::PermTraverse,
         sub_server);
    run();
    // It has a slot: the connection is served, with nothing yet to say.
    EXPECT_TRUE(fake::kernel().is_open(sub_server));
    EXPECT_EQ(fake::kernel().queued_messages(sub_client), 0u);

    auto const [client, service_end] = channel();
    open(sub_client, "nested", Flags::ProtocolService, service_end);
    run();
    ASSERT_EQ(calls().connects.size(), 1u);
    EXPECT_EQ(calls().connects[0].service, NESTED_SERVICE);
    EXPECT_EQ(calls().connects[0].channel, service_end);
}

TEST_F(LoopServerTest, ReadDirentsIsAnsweredOnTheConnection) {
    auto const [directory, directory_server] = serve_root();
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const encoded = ours_io::encode_read_dirents(
        bytes, ours_io::ReadDirentsRequest{.txid = 1, .max_bytes = 512});
    ASSERT_TRUE(encoded.is_ok());
    write(directory, encoded.unwrap());
    run();

    auto const reply = receive(directory);
    ASSERT_TRUE(reply.has_value());
    auto const decoded =
        ours_io::decode_read_dirents_reply(wire::Message{.bytes = *reply, .handles = {}}, 1);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded.unwrap().status, Status::Ok);
    EXPECT_FALSE(decoded.unwrap().dirents.empty());
    // The connection stays for the next request.
    EXPECT_TRUE(fake::kernel().is_open(directory_server));
    EXPECT_TRUE(calls().write_failures.empty());
}

TEST_F(LoopServerTest, EachConnectionIsReachedThroughItsOwnBinding) {
    // Two connections on the same directory: two slots, and a Binding for
    // each, which is the Waiter its slot calls. A request on either is
    // answered on that connection's own channel, under its own txid. Were
    // both slots to wait on one Binding, or a Binding to name the other
    // connection, the connection served last would answer for both, and the
    // first would get nothing.
    auto const [first, first_server] = serve_root();
    auto const [second, second_server] = serve_root();
    request_dirents(first, 1);
    request_dirents(second, 2);
    run();

    expect_dirents_reply(first, 1);
    expect_dirents_reply(second, 2);
    // Both stay for the next request.
    EXPECT_TRUE(fake::kernel().is_open(first_server));
    EXPECT_TRUE(fake::kernel().is_open(second_server));
    EXPECT_TRUE(calls().write_failures.empty());
}

TEST_F(LoopServerTest, CloseIsAnsweredAndEndsTheConnection) {
    auto const [directory, directory_server] = serve_root();
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const encoded = ours_io::encode_close(bytes, ours_io::CloseRequest{.txid = 1});
    ASSERT_TRUE(encoded.is_ok());
    write(directory, encoded.unwrap());
    run();

    auto const reply = receive(directory);
    ASSERT_TRUE(reply.has_value());
    auto const decoded =
        ours_io::decode_close_reply(wire::Message{.bytes = *reply, .handles = {}}, 1);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded.unwrap().status, Status::Ok);
    EXPECT_FALSE(fake::kernel().is_open(directory_server));
}

TEST_F(LoopServerTest, MalformedMessageDropsTheConnection) {
    auto const [directory, directory_server] = serve_root();
    auto const [stray, stray_peer] = channel();
    std::array<u8, 8> garbage{};
    auto const handles = std::array{stray};
    write(directory, wire::Message{.bytes = garbage, .handles = handles});
    run();

    // No epitaph, and the handle the message carried is closed with it.
    EXPECT_EQ(epitaph(directory), Status::PeerClosed);
    EXPECT_FALSE(fake::kernel().is_open(directory_server));
    EXPECT_FALSE(fake::kernel().is_open(stray));
}

TEST_F(LoopServerTest, ReplyThatCannotBeWrittenIsReportedAndEndsTheConnection) {
    auto const [directory, directory_server] = serve_root();
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const encoded = ours_io::encode_read_dirents(
        bytes, ours_io::ReadDirentsRequest{.txid = 1, .max_bytes = 512});
    ASSERT_TRUE(encoded.is_ok());
    write(directory, encoded.unwrap());
    // The client goes away with its request still queued: the server reads
    // it, and its answer has nowhere to go.
    EXPECT_EQ(ours::handle_close(directory), Status::Ok);
    run();

    EXPECT_EQ(calls().write_failures, std::vector<Status>{Status::PeerClosed});
    EXPECT_FALSE(fake::kernel().is_open(directory_server));
}

TEST_F(LoopServerTest, ClientGoneReleasesTheConnection) {
    // CONNECTIONS connections, each closed by its client in turn: if a closed
    // one kept its connection, the last serve() would find none free.
    for (auto round = usize{0}; round <= CONNECTIONS; ++round) {
        auto const [client, server_end] = serve_root();
        EXPECT_EQ(ours::handle_close(client), Status::Ok);
        run();
        EXPECT_FALSE(fake::kernel().is_open(server_end));
    }
}

// The Loop frees a connection's slot, and closes its channel, when it cannot
// arm the wait that follows a reply. The connection is released with it: it
// is no more served than the slot it had, and a connection kept for a slot
// that is gone would hold one of the program's CONNECTIONS for ever. This is
// the repair item's acceptance: CONNECTIONS connections are served after it.
TEST_F(LoopServerTest, ConnectionIsReleasedWhenTheLoopCannotArmItsWaitAgain) {
    auto const [directory, directory_server] = serve_root();
    request_dirents(directory, 1);
    // The next object_wait_async is the re-arm after the reply.
    fake::kernel().fail_next_wait_async(Status::NoMemory);
    run();

    // The reply was written before the wait failed, and the Loop closed the
    // channel with the slot.
    expect_dirents_reply(directory, 1);
    EXPECT_FALSE(fake::kernel().is_open(directory_server));

    // Every connection is free: were the failed one still held, the last of
    // these would find none.
    for (auto index = usize{0}; index < CONNECTIONS; ++index) {
        serve_root();
    }
    auto const [client, server_end] = channel();
    EXPECT_EQ(server().serve(server_end, m_root, ours_io::R_STAR_DIR), Status::NoResource);
}

// Only the connection whose wait failed is released: the other, on the same
// directory, keeps its entry and its slot and goes on answering.
TEST_F(LoopServerTest, OnlyTheConnectionWhoseRearmFailedIsReleased) {
    auto const [first, first_server] = serve_root();
    auto const [second, second_server] = serve_root();
    request_dirents(first, 1);
    fake::kernel().fail_next_wait_async(Status::BadHandle);
    run();
    expect_dirents_reply(first, 1);
    EXPECT_FALSE(fake::kernel().is_open(first_server));
    EXPECT_TRUE(fake::kernel().is_open(second_server));

    request_dirents(second, 2);
    run();
    expect_dirents_reply(second, 2);

    // One entry is free again, the second's is not.
    for (auto index = usize{0}; index + 1 < CONNECTIONS; ++index) {
        serve_root();
    }
    auto const [client, server_end] = channel();
    EXPECT_EQ(server().serve(server_end, m_root, ours_io::R_STAR_DIR), Status::NoResource);
}

TEST_F(LoopServerTest, ConnectionsPastTheBindingsAreRefused) {
    // Two connections but one Binding: the second has nothing to be waited
    // on with.
    std::array<ioservice::Connection, 2> connections{};
    std::array<ioservice::LoopServer::Binding, 1> bindings{};
    auto memory = this->memory();
    memory.connections = connections;
    memory.bindings = bindings;
    auto small = ioservice::LoopServer(tree(), event_loop(), syscalls(), memory);

    auto const first = channel();
    EXPECT_EQ(small.serve(first[1], m_root, ours_io::R_STAR_DIR), Status::Ok);
    auto const second = channel();
    EXPECT_EQ(small.serve(second[1], m_root, ours_io::R_STAR_DIR), Status::NoResource);
    // Refused, so still the caller's.
    EXPECT_TRUE(fake::kernel().is_open(second[1]));
}
} // namespace
