/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// ioservice::LoopServer on a real Loop, against the fake kernel of
// fake_kernel.hpp: what serve() refuses, and what each kind of message on a
// served directory comes to -- an Open of a service node handed to the
// connect function, an Open that walks to a Subtree node handed to the
// delegate's subtree function with the rest of its path, its flags and its
// channel, an Open of a name that is not there answered NotFound
// and reported to the delegate, a directory opened through it, a reply
// written back, a Close answered, a malformed message, a reply that cannot
// be written, a client gone, and a Loop that cannot arm a connection's wait
// again. And a Handler node whose NodeHandler answers Pending: the connection
// is suspended, complete() writes the reply and resumes it, the replies come in
// the order the requests were sent, a client that goes away while its request
// waits is released by its own callback after the resume, and a pending Open
// makes its connection when it is completed.
#include <ioservice/loop_server.hpp>

#include <algorithm>
#include <array>
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
using ours::Status;
using ours::Uandle;
using prot::io::Flags;

/// The connections the server allows at once.
constexpr auto CONNECTIONS = usize{4};

/// The value each service node is added with.
constexpr auto ECHO_SERVICE = u64{7};
constexpr auto NESTED_SERVICE = u64{9};

/// The value the Subtree node /mount is added with.
constexpr auto MOUNT_VALUE = u64{11};

/// The value the Handler node /fs is added with, and what its handler serves:
/// a directory (cookie FS_DIRECTORY) holding one file (cookie FS_FILE) named
/// "file".
constexpr auto FS_VALUE = u64{21};
constexpr auto FS_DIRECTORY = u64{1};
constexpr auto FS_FILE = u64{2};
constexpr auto FS_TEXT = std::string_view{"0123456789"};

/// The server's delegate, which keeps what it was called with, in order.
class Calls final: public ioservice::LoopServer::Delegate {
  public:
    struct Connect {
        u64 service;
        Uandle channel;
    };

    /// An Open that walked to a Subtree node, as the delegate was told it.
    struct Subtree {
        u64 value;
        /// A copy of the rest, made in the call: the view is only good there.
        std::string rest;
        /// The view itself, kept to show that it does not last.
        std::string_view viewed;
        Flags flags;
        Uandle channel;
    };

    /// An Open the server reported as refused.
    struct Refusal {
        ioservice::NodeId directory;
        std::string path;
        Status status;
        /// How many messages were queued on |watched| when it was reported:
        /// the epitaph is there already if the answer came first.
        usize queued_on_watched;
    };

    std::vector<Connect> connects;
    std::vector<Subtree> subtrees;
    std::vector<Status> write_failures;
    std::vector<Refusal> refusals;
    /// The client end of the channel the test expects an epitaph on.
    Uandle watched = Uandle::Invalid;

    auto connect(u64 const service, Uandle const channel) -> void override {
        connects.push_back({.service = service, .channel = channel});
    }

    auto subtree(u64 const value, std::string_view const rest, Flags const flags,
                 Uandle const channel) -> void override {
        subtrees.push_back({
            .value = value,
            .rest = std::string{rest},
            .viewed = rest,
            .flags = flags,
            .channel = channel,
        });
    }

    auto write_failed(Status const status) -> void override {
        write_failures.push_back(status);
    }

    auto open_refused(ioservice::NodeId const directory, std::string_view const path,
                      Status const status) -> void override {
        auto queued = usize{0};
        if (watched != Uandle::Invalid) {
            queued = fake::kernel().queued_messages(watched);
        }
        // The path is a view of the request, valid for this call only.
        refusals.push_back({
            .directory = directory,
            .path = std::string{path},
            .status = status,
            .queued_on_watched = queued,
        });
    }
};

auto bytes_of(std::string_view const text) -> std::span<u8 const> {
    return {reinterpret_cast<u8 const *>(text.data()), text.size()};
}

/// What a completed Read of |text| is.
auto read_of(std::string_view const text) -> ioservice::ReadResult {
    return ustl::ok(bytes_of(text));
}

/// The NodeHandler of /fs: a directory with one file, "file", holding FS_TEXT.
/// It answers Done unless a test says an operation waits, and then it answers
/// Pending and the test ends the request with complete(), when it likes. It
/// keeps what it was asked, so that a test can say what the Server told it and
/// when.
class Fs final: public ioservice::NodeHandler {
  public:
    struct OpenCall {
        u64 value;
        ustl::Option<u64> directory;
        std::string path;
        Flags flags;
        ioservice::Ticket ticket;
    };

    struct ReadCall {
        u64 value;
        u64 cookie;
        u64 offset;
        u64 count;
        ioservice::Ticket ticket;
    };

    // What a test sets.
    bool pend_open = false;
    bool pend_read = false;
    /// Where a call tries to complete its own ticket, when |complete_inside|.
    ioservice::LoopServer *server = nullptr;
    bool complete_inside = false;

    // What a test reads back.
    std::vector<OpenCall> opens;
    std::vector<ReadCall> reads;
    std::vector<u64> releases;
    /// What complete() answered inside the call that tried it.
    Status completed_inside = Status::Ok;

    auto open(u64 const value, ustl::Option<u64> const directory, std::string_view const path,
              Flags const flags, ioservice::Ticket const ticket)
        -> ioservice::Answer<ioservice::OpenResult> override {
        opens.push_back({value, directory, std::string{path}, flags, ticket});
        if (pend_open) {
            return ioservice::Pending{ticket};
        }
        return ioservice::Done<ioservice::OpenResult>{open_result(directory, path)};
    }

    /// What an Open of |path| from |directory| makes, as a test completes a
    /// pending one with.
    static auto open_result(ustl::Option<u64> const directory, std::string_view const path)
        -> ioservice::OpenResult {
        if (directory.is_none() && path == ".") {
            return ustl::ok(ioservice::Opened{ioservice::ObjectKind::Directory, FS_DIRECTORY});
        }
        if (directory.is_some() && *directory == FS_DIRECTORY && path == "file") {
            return ustl::ok(ioservice::Opened{ioservice::ObjectKind::File, FS_FILE});
        }
        return ustl::err(Status::NotFound);
    }

    auto read(u64 const value, u64 const cookie, u64 const offset, u64 const count,
              ioservice::Ticket const ticket) -> ioservice::Answer<ioservice::ReadResult> override {
        reads.push_back({value, cookie, offset, count, ticket});
        if (complete_inside && server != nullptr) {
            // The ticket is good once this call has returned Pending, and not
            // before: the Server refuses it, and nothing is written or resumed.
            completed_inside = server->complete(ticket, read_of("inside"));
        }
        if (pend_read) {
            return ioservice::Pending{ticket};
        }
        return ioservice::Done<ioservice::ReadResult>{read_of(slice(offset, count))};
    }

    auto read_dirents(u64, u64, u64, u64, ioservice::Ticket)
        -> ioservice::Answer<ioservice::EntriesResult> override {
        return ioservice::Done<ioservice::EntriesResult>{ustl::err(Status::NotSupported)};
    }

    auto get_backing_memory(u64, u64, prot::io::VmoFlags, ioservice::Ticket)
        -> ioservice::Answer<ioservice::VmoResult> override {
        return ioservice::Done<ioservice::VmoResult>{ustl::err(Status::NotSupported)};
    }

    auto clone(u64, u64 const cookie) -> ustl::Result<u64, Status> override {
        return ustl::ok(cookie);
    }

    auto release(u64, u64 const cookie) -> void override {
        releases.push_back(cookie);
    }

  private:
    static auto slice(u64 const offset, u64 const count) -> std::string_view {
        auto const from = std::min<usize>(offset, FS_TEXT.size());
        return FS_TEXT.substr(from, count);
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
    auto const encoded = prot::io::encode_open(
        bytes, handles, prot::io::OpenRequest{.flags = flags, .path = path, .object = object});
    ASSERT_TRUE(encoded.is_ok());
    write(directory, encoded.unwrap());
}

/// Writes ReadDirents(|txid|) on |directory|.
auto request_dirents(Uandle const directory, u32 const txid) -> void {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const encoded = prot::io::encode_read_dirents(
        bytes, prot::io::ReadDirentsRequest{.txid = txid, .max_bytes = 512});
    ASSERT_TRUE(encoded.is_ok());
    write(directory, encoded.unwrap());
}

/// Writes Read(|txid|, |count|) on the file connection |file|.
auto request_read(Uandle const file, u32 const txid, u64 const count) -> void {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const encoded =
        prot::io::encode_read(bytes, prot::io::ReadRequest{.txid = txid, .count = count});
    ASSERT_TRUE(encoded.is_ok());
    write(file, encoded.unwrap());
}

/// Writes Clone(|object|) on |directory|.
auto clone(Uandle const directory, Uandle const object) -> void {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, 1> handles{};
    auto const encoded =
        prot::io::encode_clone(bytes, handles, prot::io::CloneRequest{.request = object});
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

/// The data of the Ok answer to Read(|txid|) that is waiting on |file|, as
/// text; none when no message is, or when it is not that answer.
auto read_reply(Uandle const file, u32 const txid) -> std::optional<std::string> {
    auto const reply = receive(file);
    if (!reply.has_value()) {
        return std::nullopt;
    }
    auto const decoded =
        prot::io::decode_read_reply(wire::Message{.bytes = *reply, .handles = {}}, txid);
    if (decoded.is_err() || decoded.unwrap().status != Status::Ok) {
        return std::nullopt;
    }
    auto const data = decoded.unwrap().data;
    return std::string{data.begin(), data.end()};
}

/// Checks that the one message waiting on |directory| is the Ok answer to
/// ReadDirents(|txid|).
auto expect_dirents_reply(Uandle const directory, u32 const txid) -> void {
    auto const reply = receive(directory);
    ASSERT_TRUE(reply.has_value());
    auto const decoded =
        prot::io::decode_read_dirents_reply(wire::Message{.bytes = *reply, .handles = {}}, txid);
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
///   /mount     a Subtree node, MOUNT_VALUE
///   /fs        a Handler node, FS_VALUE, answered by Fs
class LoopServerTest: public testing::Test {
  protected:
    LoopServerTest() : m_storage(std::make_unique<Storage>()), m_tree(m_storage->nodes) {
        fake::kernel().reset();
        auto port = Uandle::Invalid;
        EXPECT_EQ(ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok);

        m_root = m_tree.add_root().unwrap();
        m_echo = m_tree.add_service(m_root, "echo", ECHO_SERVICE).unwrap();
        m_sub = m_tree.add_directory(m_root, "sub").unwrap();
        // The tests find /sub/nested by its path, so its id is not kept.
        auto _ = m_tree.add_service(m_sub, "nested", NESTED_SERVICE).unwrap();
        m_mount = m_tree.add_subtree(m_root, "mount", MOUNT_VALUE).unwrap();
        m_fs_node = m_tree.add_handler(m_root, "fs", FS_VALUE).unwrap();

        m_loop.emplace(port, m_slots);
        m_server.emplace(m_tree, *m_loop, syscalls(), memory(), &m_calls, &m_fs);
        m_fs.server = &*m_server;
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

    auto fs() -> Fs & {
        return m_fs;
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
        EXPECT_EQ(server().serve(ends[1], m_root, prot::io::R_STAR_DIR), Status::Ok);
        return ends;
    }

    /// The buffer the server reads each request into.
    auto request_bytes() -> std::span<u8> {
        return m_storage->request_bytes;
    }

    ioservice::NodeId m_root = {};
    ioservice::NodeId m_echo = {};
    ioservice::NodeId m_sub = {};
    ioservice::NodeId m_mount = {};
    ioservice::NodeId m_fs_node = {};

  private:
    std::array<loop::Slot, 8> m_slots{};
    std::unique_ptr<Storage> m_storage;
    ioservice::Tree m_tree;
    std::optional<loop::Loop> m_loop;
    std::optional<ioservice::LoopServer> m_server;
    Calls m_calls;
    Fs m_fs;
};

TEST_F(LoopServerTest, ServeRefusals) {
    EXPECT_EQ(server().serve(Uandle::Invalid, m_root, prot::io::R_STAR_DIR), Status::InvArgs);

    // A service is not a directory, and nothing is served.
    auto const [client, server_end] = channel();
    EXPECT_EQ(server().serve(server_end, m_echo, prot::io::R_STAR_DIR), Status::NotDir);
    EXPECT_TRUE(fake::kernel().is_open(server_end));
}

TEST_F(LoopServerTest, ServeRefusesPastItsConnections) {
    for (auto index = usize{0}; index < CONNECTIONS; ++index) {
        serve_root();
    }
    auto const [client, server_end] = channel();
    EXPECT_EQ(server().serve(server_end, m_root, prot::io::R_STAR_DIR), Status::NoResource);
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

/// A delegate that serves service nodes and nothing else: it does not
/// override subtree().
class ConnectOnly final: public ioservice::LoopServer::Delegate {
  public:
    auto connect(u64, Uandle) -> void override {}
};

TEST_F(LoopServerTest, OpenThatWalksToASubtreeNodeReachesTheDelegateWithWhatItWasAskedFor) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, object] = channel();
    constexpr auto FLAGS = Flags::ProtocolDirectory | prot::io::PERM_READABLE;
    open(directory, "mount/a/b", FLAGS, object);
    run();

    ASSERT_EQ(calls().subtrees.size(), 1u);
    EXPECT_EQ(calls().subtrees[0].value, MOUNT_VALUE);
    EXPECT_EQ(calls().subtrees[0].rest, "a/b");
    EXPECT_EQ(calls().subtrees[0].flags, FLAGS);
    EXPECT_EQ(calls().subtrees[0].channel, object);
    // The delegate owns the channel: nothing was written on it or closed, and
    // the directory's connection stays for the next Open. It is no service
    // and no refusal.
    EXPECT_TRUE(fake::kernel().is_open(object));
    EXPECT_EQ(fake::kernel().queued_messages(client), 0u);
    EXPECT_TRUE(fake::kernel().is_open(directory_server));
    EXPECT_TRUE(calls().connects.empty());
    EXPECT_TRUE(calls().refusals.empty());
}

TEST_F(LoopServerTest, TheRestIsDotWhenTheOpenNamesTheSubtreeNodeItself) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, object] = channel();
    open(directory, "mount", Flags::ProtocolDirectory | prot::io::PERM_READABLE, object);
    run();

    ASSERT_EQ(calls().subtrees.size(), 1u);
    EXPECT_EQ(calls().subtrees[0].rest, ".");
}

TEST_F(LoopServerTest, OpenOfASubtreeNodeMakesNoConnectionAndNoSlot) {
    auto const [directory, directory_server] = serve_root();
    // Many more Opens than there are connections: none takes one.
    for (auto count = usize{0}; count < 2 * CONNECTIONS; ++count) {
        auto const [client, object] = channel();
        open(directory, "mount/x", prot::io::PERM_READABLE, object);
        run();
    }
    EXPECT_EQ(calls().subtrees.size(), 2 * CONNECTIONS);

    // The table has all its other entries: three more directories fit, and the
    // next does not.
    for (auto index = usize{1}; index < CONNECTIONS; ++index) {
        serve_root();
    }
    auto const [client, server_end] = channel();
    EXPECT_EQ(server().serve(server_end, m_root, prot::io::R_STAR_DIR), Status::NoResource);
}

TEST_F(LoopServerTest, TheRestViewsTheRequestAndDoesNotLastPastTheCall) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, first] = channel();
    open(directory, "mount/abc", prot::io::PERM_READABLE, first);
    run();
    ASSERT_EQ(calls().subtrees.size(), 1u);

    // The view is inside the buffer the server reads each request into,
    // which is why a handler that parks the Open copies it.
    auto const buffer = request_bytes();
    auto const viewed = calls().subtrees[0].viewed;
    EXPECT_GE(viewed.data(), reinterpret_cast<char const *>(buffer.data()));
    EXPECT_LE(viewed.data() + viewed.size(),
              reinterpret_cast<char const *>(buffer.data() + buffer.size()));
    EXPECT_EQ(viewed, "abc");

    // The next request is read over it.
    auto const [other_client, second] = channel();
    open(directory, "mount/XYZ", prot::io::PERM_READABLE, second);
    run();
    EXPECT_EQ(viewed, "XYZ");
    EXPECT_EQ(calls().subtrees[0].rest, "abc");
    EXPECT_EQ(calls().subtrees[1].rest, "XYZ");
}

TEST_F(LoopServerTest, ADelegateThatParksAnOpenCanAnswerItLaterOnItsChannel) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, object] = channel();
    open(directory, "mount/parked", prot::io::PERM_READABLE, object);
    run();
    ASSERT_EQ(calls().subtrees.size(), 1u);

    // Other traffic goes by first, and rewrites the request buffer. The server
    // takes nothing from the parked channel meanwhile.
    auto const [echo_client, echo_end] = channel();
    open(directory, "echo", Flags::ProtocolService, echo_end);
    request_dirents(directory, 1);
    run();
    expect_dirents_reply(directory, 1);
    EXPECT_EQ(fake::kernel().queued_messages(client), 0u);
    EXPECT_TRUE(fake::kernel().is_open(object));

    // The answer, much later: the parked channel and the copied rest are all
    // it needs.
    EXPECT_EQ(calls().subtrees[0].rest, "parked");
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const epitaph_message = wire::encode_epitaph(bytes, Status::NotSupported);
    ASSERT_TRUE(epitaph_message.is_ok());
    write(calls().subtrees[0].channel, epitaph_message.unwrap());
    EXPECT_EQ(epitaph(client), Status::NotSupported);
}

TEST_F(LoopServerTest, ADelegateThatDoesNotOverrideSubtreeClosesTheChannel) {
    ConnectOnly connect_only;
    server().set_delegate(&connect_only);
    auto const [directory, directory_server] = serve_root();
    auto const [client, object] = channel();
    open(directory, "mount/x", prot::io::PERM_READABLE, object);
    run();

    // Nobody else would: the server gave the channel away, and the default
    // takes it.
    EXPECT_FALSE(fake::kernel().is_open(object));
    EXPECT_EQ(epitaph(client), Status::PeerClosed);
    EXPECT_TRUE(fake::kernel().is_open(directory_server));
}

TEST_F(LoopServerTest, OpenOfASubtreeNodeWithoutADelegateClosesItsChannel) {
    server().set_delegate(nullptr);
    auto const [directory, directory_server] = serve_root();
    auto const [client, object] = channel();
    open(directory, "mount/x", prot::io::PERM_READABLE, object);
    run();

    EXPECT_FALSE(fake::kernel().is_open(object));
    EXPECT_EQ(epitaph(client), Status::PeerClosed);
    EXPECT_TRUE(calls().subtrees.empty());
    EXPECT_TRUE(fake::kernel().is_open(directory_server));
}

TEST_F(LoopServerTest, AnOpenTheConnectionRefusesNeverReachesTheDelegate) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, object] = channel();
    calls().watched = client;
    // The connection is r*: it holds no write, so the Open is refused before
    // the walk reaches the node.
    open(directory, "mount/x", Flags::PermWriteBytes, object);
    run();

    EXPECT_TRUE(calls().subtrees.empty());
    EXPECT_EQ(epitaph(client), Status::AccessDenied);
    EXPECT_FALSE(fake::kernel().is_open(object));
    ASSERT_EQ(calls().refusals.size(), 1u);
    EXPECT_EQ(calls().refusals[0].path, "mount/x");
    EXPECT_EQ(calls().refusals[0].status, Status::AccessDenied);
}

TEST_F(LoopServerTest, SubtreeOpensAreHandedOverInTheOrderTheyArrived) {
    auto const [directory, directory_server] = serve_root();
    auto objects = std::vector<Uandle>{};
    for (auto const rest: {"one", "two", "three"}) {
        auto const [client, object] = channel();
        objects.push_back(object);
        open(directory, std::string{"mount/"} + rest, prot::io::PERM_READABLE, object);
    }
    run();

    ASSERT_EQ(calls().subtrees.size(), 3u);
    EXPECT_EQ(calls().subtrees[0].rest, "one");
    EXPECT_EQ(calls().subtrees[1].rest, "two");
    EXPECT_EQ(calls().subtrees[2].rest, "three");
    for (auto index = usize{0}; index < 3; ++index) {
        EXPECT_EQ(calls().subtrees[index].channel, objects[index]);
    }
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

TEST_F(LoopServerTest, RefusedOpenIsReportedWithItsDirectoryPathAndStatus) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, service_end] = channel();
    calls().watched = client;
    open(directory, "missing", Flags::ProtocolService, service_end);
    run();

    ASSERT_EQ(calls().refusals.size(), 1u);
    EXPECT_EQ(calls().refusals[0].directory, m_root);
    EXPECT_EQ(calls().refusals[0].path, "missing");
    EXPECT_EQ(calls().refusals[0].status, Status::NotFound);
    // Reported after the client was answered, not before: its epitaph was
    // already queued when the delegate was told.
    EXPECT_EQ(calls().refusals[0].queued_on_watched, 1u);
    // The answer is what it was without a report.
    EXPECT_EQ(epitaph(client), Status::NotFound);
    EXPECT_TRUE(fake::kernel().is_open(directory_server));
}

TEST_F(LoopServerTest, RefusedOpenNamesTheDirectoryItWasMadeOn) {
    auto const [directory, directory_server] = serve_root();
    auto const [sub_client, sub_server] = channel();
    open(directory, "sub", Flags::ProtocolDirectory | Flags::PermConnect | Flags::PermTraverse,
         sub_server);
    auto const [client, service_end] = channel();
    open(sub_client, "absent", Flags::ProtocolService, service_end);
    run();

    // The first Open succeeded and is not reported; the second was made on
    // /sub, and says so.
    ASSERT_EQ(calls().refusals.size(), 1u);
    EXPECT_EQ(calls().refusals[0].directory, m_sub);
    EXPECT_EQ(calls().refusals[0].path, "absent");
    EXPECT_EQ(calls().refusals[0].status, Status::NotFound);
}

TEST_F(LoopServerTest, RefusedOpenWithAnInvalidPathIsReportedAsSent) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, service_end] = channel();
    // An empty path encodes, and the server refuses it: it is still a path
    // that was asked for, and is reported, empty as it is.
    open(directory, "", Flags::ProtocolService, service_end);
    run();

    ASSERT_EQ(calls().refusals.size(), 1u);
    EXPECT_EQ(calls().refusals[0].path, "");
    EXPECT_EQ(calls().refusals[0].status, Status::BadPath);
    EXPECT_EQ(epitaph(client), Status::BadPath);
}

TEST_F(LoopServerTest, AnOpenThatSucceedsIsNotReported) {
    auto const [directory, directory_server] = serve_root();
    auto const [client, service_end] = channel();
    open(directory, "echo", Flags::ProtocolService, service_end);
    auto const [sub_client, sub_server] = channel();
    open(directory, "sub", Flags::ProtocolDirectory | Flags::PermConnect | Flags::PermTraverse,
         sub_server);
    run();

    EXPECT_EQ(calls().connects.size(), 1u);
    EXPECT_TRUE(calls().refusals.empty());
}

TEST_F(LoopServerTest, RefusedCloneIsNotReported) {
    // Every connection taken, so a Clone is refused with NoResource: it asks
    // for no path, and the delegate is not told.
    auto const [directory, directory_server] = serve_root();
    for (auto index = usize{1}; index < CONNECTIONS; ++index) {
        serve_root();
    }
    auto const [client, cloned] = channel();
    clone(directory, cloned);
    run();

    EXPECT_EQ(epitaph(client), Status::NoResource);
    EXPECT_TRUE(calls().refusals.empty());
}

TEST_F(LoopServerTest, RefusedOpenWithoutADelegateIsStillAnswered) {
    server().set_delegate(nullptr);
    auto const [directory, directory_server] = serve_root();
    auto const [client, service_end] = channel();
    open(directory, "missing", Flags::ProtocolService, service_end);
    run();

    EXPECT_EQ(epitaph(client), Status::NotFound);
    EXPECT_TRUE(calls().refusals.empty());
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
    auto const encoded = prot::io::encode_read_dirents(
        bytes, prot::io::ReadDirentsRequest{.txid = 1, .max_bytes = 512});
    ASSERT_TRUE(encoded.is_ok());
    write(directory, encoded.unwrap());
    run();

    auto const reply = receive(directory);
    ASSERT_TRUE(reply.has_value());
    auto const decoded =
        prot::io::decode_read_dirents_reply(wire::Message{.bytes = *reply, .handles = {}}, 1);
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
    auto const encoded = prot::io::encode_close(bytes, prot::io::CloseRequest{.txid = 1});
    ASSERT_TRUE(encoded.is_ok());
    write(directory, encoded.unwrap());
    run();

    auto const reply = receive(directory);
    ASSERT_TRUE(reply.has_value());
    auto const decoded =
        prot::io::decode_close_reply(wire::Message{.bytes = *reply, .handles = {}}, 1);
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
    auto const encoded = prot::io::encode_read_dirents(
        bytes, prot::io::ReadDirentsRequest{.txid = 1, .max_bytes = 512});
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
    EXPECT_EQ(server().serve(server_end, m_root, prot::io::R_STAR_DIR), Status::NoResource);
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
    EXPECT_EQ(server().serve(server_end, m_root, prot::io::R_STAR_DIR), Status::NoResource);
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
    EXPECT_EQ(small.serve(first[1], m_root, prot::io::R_STAR_DIR), Status::Ok);
    auto const second = channel();
    EXPECT_EQ(small.serve(second[1], m_root, prot::io::R_STAR_DIR), Status::NoResource);
    // Refused, so still the caller's.
    EXPECT_TRUE(fake::kernel().is_open(second[1]));
}

// --- forwarding through a remote entry --------------------------------------------

TEST_F(LoopServerTest, OpenThroughARemoteEntryIsForwardedOnItsChannel) {
    // The pair the mount point sits on: the tree holds one end and the
    // forwarded Open is written there; the server behind the mount reads the
    // other.
    auto const link = channel();
    ASSERT_TRUE(tree().add_remote(m_root, "boot", link[0]).is_ok());

    auto const ends = serve_root();
    auto const object = channel();
    open(ends[0], "boot/bin/x", prot::io::PERM_READABLE, object[1]);
    run();

    // The Open arrived on the remote's end of the link, the rest of its path
    // and the client's server end carried with it, the flags unchanged.
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    ASSERT_EQ(ours::channel_read(link[1], ours::ChannelReadOptions::None, bytes.data(),
                                 handles.data(), static_cast<u32>(bytes.size()),
                                 static_cast<u32>(handles.size()), &num_bytes, &num_handles),
              Status::Ok);
    auto const forwarded = prot::io::decode_open(wire::Message{
        .bytes = std::span{bytes}.first(num_bytes),
        .handles = std::span{handles}.first(num_handles),
    });
    ASSERT_TRUE(forwarded.is_ok());
    EXPECT_EQ(forwarded->path, "bin/x");
    EXPECT_EQ(forwarded->flags, prot::io::PERM_READABLE);
    EXPECT_EQ(forwarded->object, object[1]);
    // Nothing came back on the client's connection: forwarding keeps no
    // per-open state.
    EXPECT_EQ(fake::kernel().queued_messages(ends[0]), 0U);
}

TEST_F(LoopServerTest, ForwardToADeadRemoteConsumesTheOpen) {
    auto const link = channel();
    ASSERT_TRUE(tree().add_remote(m_root, "boot", link[0]).is_ok());
    // The server behind the mount is gone.
    auto _ = ours::handle_close(link[1]);

    auto const ends = serve_root();
    auto const object = channel();
    open(ends[0], "boot/x", prot::io::PERM_READABLE, object[1]);
    run();

    // The failed write consumed the client's server end, so the client sees
    // its channel close with no epitaph on it, as an Open routed into a dead
    // provider already does.
    EXPECT_FALSE(fake::kernel().is_open(object[1]));
    EXPECT_EQ(fake::kernel().queued_messages(ends[0]), 0U);
}

// --- a Handler node whose handler answers Pending -----------------------------------

/// /fs opened through the root directory, the handler answering at once unless
/// a test says otherwise. The Loop's slots are taken in the order the
/// connections are made, which the keys below rely on: the root directory is
/// slot 0, /fs slot 1, and the files opened below it slots 2 and up, all at
/// generation 0.
class PendingTest: public LoopServerTest {
  protected:
    static constexpr auto DIRECTORY_SLOT = u32{1};
    static constexpr auto FIRST_FILE_SLOT = u32{2};

    /// The two ends of a file connection.
    struct Ends {
        Uandle client;
        Uandle server;
    };

    /// Serves the root directory and opens /fs through it: the handler's
    /// directory, as a connection of its own.
    auto open_fs() -> void {
        m_root_client = serve_root()[0];
        auto const [client, server_end] = channel();
        open(m_root_client, "fs", Flags::ProtocolDirectory | prot::io::PERM_READABLE, server_end);
        run();
        m_dir_client = client;
        m_dir_server = server_end;
    }

    /// Opens the file below /fs. The Open is one-way, so the caller may write on
    /// the client end at once.
    auto open_file() -> Ends {
        auto const [client, server_end] = channel();
        open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, server_end);
        run();
        return {client, server_end};
    }

    /// How many more connections the table takes: serves the root directory
    /// until it is refused, and counts.
    auto free_connections() -> usize {
        auto count = usize{0};
        for (;;) {
            auto const [client, server_end] = channel();
            if (server().serve(server_end, m_root, prot::io::R_STAR_DIR) != Status::Ok) {
                return count;
            }
            ++count;
        }
    }

    Uandle m_root_client = Uandle::Invalid;
    Uandle m_dir_client = Uandle::Invalid;
    Uandle m_dir_server = Uandle::Invalid;
};

TEST_F(PendingTest, APendingReadIsNotAnsweredNowAndCompleteAnswersIt) {
    fs().pend_read = true;
    open_fs();
    auto const file = open_file();
    request_read(file.client, 1, 4);
    run();

    // The handler was asked once, for what the Server has checked and keeps.
    ASSERT_EQ(fs().reads.size(), 1u);
    EXPECT_EQ(fs().reads[0].value, FS_VALUE);
    EXPECT_EQ(fs().reads[0].cookie, FS_FILE);
    EXPECT_EQ(fs().reads[0].offset, 0u);
    EXPECT_EQ(fs().reads[0].count, 4u);
    // Nothing is answered now, and the connection stays.
    EXPECT_EQ(fake::kernel().queued_messages(file.client), 0u);
    EXPECT_TRUE(fake::kernel().is_open(file.server));

    // complete() writes the reply itself.
    EXPECT_EQ(server().complete(fs().reads[0].ticket, read_of("0123")), Status::Ok);
    EXPECT_EQ(fake::kernel().queued_messages(file.client), 1u);
    EXPECT_EQ(read_reply(file.client, 1).value_or("<none>"), "0123");
    run();
    EXPECT_TRUE(fake::kernel().is_open(file.server));
    EXPECT_TRUE(fs().releases.empty());
    EXPECT_TRUE(calls().write_failures.empty());
}

TEST_F(PendingTest, ASecondRequestWrittenBehindAPendingOneIsServedAfterItAndRepliesComeInOrder) {
    fs().pend_read = true;
    open_fs();
    auto const file = open_file();
    request_read(file.client, 1, 4);
    request_read(file.client, 2, 3);
    run();

    // Only the first was started. The second is in the channel, unread, and
    // another turn of the Loop does not read it: a suspended slot waits on
    // nothing.
    ASSERT_EQ(fs().reads.size(), 1u);
    EXPECT_EQ(fake::kernel().queued_messages(file.server), 1u);
    run();
    EXPECT_EQ(fs().reads.size(), 1u);
    auto const first = fs().reads[0].ticket;

    // The first is answered, and the second is not started inside complete():
    // it starts when the Loop reads the connection again, with the offset the
    // completed Read moved.
    EXPECT_EQ(server().complete(first, read_of("0123")), Status::Ok);
    EXPECT_EQ(fs().reads.size(), 1u);
    run();
    ASSERT_EQ(fs().reads.size(), 2u);
    EXPECT_EQ(fs().reads[1].offset, 4u);
    EXPECT_EQ(fs().reads[1].count, 3u);
    EXPECT_NE(fs().reads[1].ticket, first);
    EXPECT_EQ(server().complete(fs().reads[1].ticket, read_of("456")), Status::Ok);

    // In the order they were sent.
    EXPECT_EQ(read_reply(file.client, 1).value_or("<none>"), "0123");
    EXPECT_EQ(read_reply(file.client, 2).value_or("<none>"), "456");
}

TEST_F(PendingTest, TwoPendingConnectionsAreCompletedInAnyOrderEachOnItsOwnChannel) {
    fs().pend_read = true;
    open_fs();
    auto const first = open_file();
    auto const second = open_file();
    request_read(first.client, 1, 2);
    request_read(second.client, 7, 3);
    run();
    ASSERT_EQ(fs().reads.size(), 2u);

    // The later request is completed first. Its answer is on its own channel,
    // under its own txid, and the other connection is still waiting.
    EXPECT_EQ(server().complete(fs().reads[1].ticket, read_of("abc")), Status::Ok);
    EXPECT_FALSE(read_reply(first.client, 1).has_value());
    EXPECT_EQ(read_reply(second.client, 7).value_or("<none>"), "abc");
    EXPECT_EQ(server().complete(fs().reads[0].ticket, read_of("de")), Status::Ok);
    EXPECT_EQ(read_reply(first.client, 1).value_or("<none>"), "de");

    // Each connection's slot was resumed with its own key: both go on.
    request_read(first.client, 2, 1);
    request_read(second.client, 8, 1);
    run();
    EXPECT_EQ(fs().reads.size(), 4u);
}

TEST_F(PendingTest, AnotherConnectionIsServedWhileOneIsPending) {
    fs().pend_read = true;
    open_fs();
    auto const file = open_file();
    request_read(file.client, 1, 4);
    request_dirents(m_root_client, 5);
    run();

    // The root directory was answered while the file's Read waits.
    expect_dirents_reply(m_root_client, 5);
    ASSERT_EQ(fs().reads.size(), 1u);
    EXPECT_EQ(fake::kernel().queued_messages(file.client), 0u);
}

TEST_F(PendingTest, AClientThatClosesWhilePendingGetsNoReplyAndIsReleasedByItsOwnCallback) {
    fs().pend_read = true;
    open_fs();
    auto const file = open_file();
    request_read(file.client, 1, 4);
    run();
    ASSERT_EQ(fs().reads.size(), 1u);
    auto const ticket = fs().reads[0].ticket;
    EXPECT_EQ(ours::handle_close(file.client), Status::Ok);
    run();

    // Nobody sees the client go: the slot is suspended, with no wait armed.
    EXPECT_TRUE(fake::kernel().is_open(file.server));
    EXPECT_TRUE(fs().releases.empty());

    // The handler finishes, as it must; the write fails, the delegate is told,
    // and complete() leaves the connection alone.
    EXPECT_EQ(server().complete(ticket, read_of("0123")), Status::Ok);
    EXPECT_EQ(calls().write_failures, std::vector<Status>{Status::PeerClosed});
    EXPECT_TRUE(fake::kernel().is_open(file.server));
    EXPECT_TRUE(fs().releases.empty());
    // Its entry is still taken, so that no new connection can take its index
    // while the old slot is in use: the table is the root's, /fs's, this one,
    // and one more.
    EXPECT_EQ(free_connections(), 1u);

    // The resumed slot's own callback finds the peer closed and nothing to
    // read, releases the connection, and the Loop closes the slot.
    run();
    EXPECT_FALSE(fake::kernel().is_open(file.server));
    EXPECT_EQ(fs().releases, std::vector<u64>{FS_FILE});

    // The ticket is spent, and so is the connection: nothing is touched.
    EXPECT_EQ(server().complete(ticket, read_of("0123")), Status::NotFound);
    EXPECT_EQ(fs().releases.size(), 1u);
    EXPECT_EQ(calls().write_failures.size(), 1u);
}

TEST_F(PendingTest, AClientThatClosesWithAnotherRequestQueuedIsReleasedOnceAfterBothAreCompleted) {
    fs().pend_read = true;
    open_fs();
    auto const file = open_file();
    request_read(file.client, 1, 4);
    request_read(file.client, 2, 3);
    run();
    EXPECT_EQ(ours::handle_close(file.client), Status::Ok);

    EXPECT_EQ(server().complete(fs().reads[0].ticket, read_of("0123")), Status::Ok);
    // The closed peer and the second request are both there when the slot is
    // read again: the request is read, and started, though its reply has
    // nowhere to go.
    run();
    ASSERT_EQ(fs().reads.size(), 2u);
    EXPECT_TRUE(fs().releases.empty());
    EXPECT_EQ(server().complete(fs().reads[1].ticket, read_of("456")), Status::Ok);
    EXPECT_EQ(calls().write_failures.size(), 2u);
    run();
    EXPECT_FALSE(fake::kernel().is_open(file.server));
    EXPECT_EQ(fs().releases, std::vector<u64>{FS_FILE});
}

TEST_F(PendingTest, ASpentOrMismatchedTicketIsRefusedAndWritesAndResumesNothing) {
    fs().pend_read = true;
    open_fs();
    auto const file = open_file();
    request_read(file.client, 1, 4);
    run();
    auto const ticket = fs().reads[0].ticket;
    EXPECT_EQ(server().complete(ticket, read_of("0123")), Status::Ok);
    EXPECT_EQ(read_reply(file.client, 1).value_or("<none>"), "0123");

    // Spent: refused, and no second reply, whatever it says.
    EXPECT_EQ(server().complete(ticket, read_of("zzzz")), Status::NotFound);
    EXPECT_EQ(fake::kernel().queued_messages(file.client), 0u);

    // A pending Read completed with another operation's result: refused, and
    // the request is still pending, so the right result ends it.
    request_read(file.client, 2, 2);
    run();
    ASSERT_EQ(fs().reads.size(), 2u);
    EXPECT_EQ(server().complete(fs().reads[1].ticket,
                                ioservice::EntriesResult{ustl::err(Status::NotSupported)}),
              Status::InvArgs);
    EXPECT_EQ(fake::kernel().queued_messages(file.client), 0u);
    EXPECT_EQ(server().complete(fs().reads[1].ticket, read_of("45")), Status::Ok);
    EXPECT_EQ(read_reply(file.client, 2).value_or("<none>"), "45");
}

TEST_F(PendingTest, CompleteSaysWhenTheSlotWasNotSuspended) {
    fs().pend_read = true;
    open_fs();
    auto const file = open_file();
    request_read(file.client, 1, 4);
    run();

    // Someone else resumed the slot of the connection that waits: it is armed.
    // The Binding left it suspended, and nothing here resumes twice.
    EXPECT_EQ(event_loop().resume(loop::Key{FIRST_FILE_SLOT, 0}), Status::Ok);
    // The request is ended and answered all the same, and the program is told
    // that the resume found the slot armed.
    EXPECT_EQ(server().complete(fs().reads[0].ticket, read_of("0123")), Status::BadState);
    EXPECT_EQ(read_reply(file.client, 1).value_or("<none>"), "0123");
    request_read(file.client, 2, 1);
    run();
    EXPECT_EQ(fs().reads.size(), 2u);
}

TEST_F(PendingTest, ACompletionInsideTheCallThatGotTheTicketIsRefusedAndTheConnectionIsServed) {
    // The one way complete() could run inside the Binding's own callback, where
    // resume() answers BadState and Suspend would leave the connection stuck:
    // the Server accepts a ticket only once the handler's call has returned
    // Pending, so the call is refused and the handler answers Done.
    fs().complete_inside = true;
    fs().server = &server();
    open_fs();
    auto const file = open_file();
    request_read(file.client, 1, 4);
    run();

    EXPECT_EQ(fs().completed_inside, Status::NotFound);
    EXPECT_EQ(read_reply(file.client, 1).value_or("<none>"), "0123");
    EXPECT_EQ(fake::kernel().queued_messages(file.client), 0u);
    // Not left suspended: the next request is read.
    request_read(file.client, 2, 2);
    run();
    EXPECT_EQ(read_reply(file.client, 2).value_or("<none>"), "45");
}

TEST_F(PendingTest, AResumeThatTheKernelRefusesReleasesTheConnectionAfterTheReplyIsOut) {
    fs().pend_read = true;
    open_fs();
    auto const file = open_file();
    request_read(file.client, 1, 4);
    run();

    // The next wait is the arm of resume(): the Loop frees the slot, closes the
    // channel and tells the Binding, which releases the connection, all inside
    // complete(). The reply was written before.
    fake::kernel().fail_next_wait_async(Status::NoMemory);
    EXPECT_EQ(server().complete(fs().reads[0].ticket, read_of("0123")), Status::Ok);
    EXPECT_EQ(read_reply(file.client, 1).value_or("<none>"), "0123");
    EXPECT_FALSE(fake::kernel().is_open(file.server));
    EXPECT_EQ(fs().releases, std::vector<u64>{FS_FILE});
    // Its entry is free: the table is the root's and /fs's, and two more.
    EXPECT_EQ(free_connections(), 2u);
}

// --- a pending Open ---------------------------------------------------------------

TEST_F(PendingTest,
       APendingOpenSuspendsItsDirectoryConnectionAndTheNewConnectionIsMadeAtCompletion) {
    open_fs();
    fs().pend_open = true;
    auto const [file_client, file_server] = channel();
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, file_server);
    // The object-to-be is written to at once, and a second Open waits behind
    // the first on the directory.
    request_read(file_client, 1, 4);
    auto const [second_client, second_server] = channel();
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, second_server);
    run();

    // opens[0] is the Open of /fs itself.
    ASSERT_EQ(fs().opens.size(), 2u);
    EXPECT_EQ(fs().opens[1].path, "file");
    EXPECT_EQ(fs().opens[1].directory.is_some(), true);
    // No connection yet: the Read is unread, the server end is held and
    // nothing is written on it.
    EXPECT_TRUE(fs().reads.empty());
    EXPECT_EQ(fake::kernel().queued_messages(file_server), 1u);
    EXPECT_TRUE(fake::kernel().is_open(file_server));
    EXPECT_EQ(fake::kernel().queued_messages(file_client), 0u);
    EXPECT_EQ(free_connections(), 2u);
}

TEST_F(PendingTest, ACompletedOpenMakesItsConnectionAndResumesTheDirectory) {
    open_fs();
    fs().pend_open = true;
    auto const [file_client, file_server] = channel();
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, file_server);
    request_read(file_client, 1, 4);
    auto const [second_client, second_server] = channel();
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, second_server);
    run();
    ASSERT_EQ(fs().opens.size(), 2u);

    EXPECT_EQ(
        server().complete(fs().opens[1].ticket, Fs::open_result(ustl::some(FS_DIRECTORY), "file")),
        Status::Ok);
    // The connection is made on the Open's server end, and waits on it; the
    // directory's slot is resumed, and the Open behind starts, which the
    // handler again leaves pending.
    run();
    ASSERT_EQ(fs().reads.size(), 1u);
    EXPECT_EQ(fs().reads[0].cookie, FS_FILE);
    EXPECT_EQ(read_reply(file_client, 1).value_or("<none>"), "0123");
    ASSERT_EQ(fs().opens.size(), 3u);
    EXPECT_TRUE(fake::kernel().is_open(second_server));
    EXPECT_TRUE(fs().releases.empty());
    // root, /fs and the new file: one more fits.
    EXPECT_EQ(free_connections(), 1u);
}

TEST_F(PendingTest, ARefusedPendingOpenIsAnsweredWithAnEpitaphAndNotReported) {
    open_fs();
    fs().pend_open = true;
    auto const [file_client, file_server] = channel();
    calls().watched = file_client;
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, file_server);
    run();
    ASSERT_EQ(fs().opens.size(), 2u);

    EXPECT_EQ(
        server().complete(fs().opens[1].ticket, ioservice::OpenResult{ustl::err(Status::NotFound)}),
        Status::Ok);
    EXPECT_EQ(epitaph(file_client), Status::NotFound);
    EXPECT_FALSE(fake::kernel().is_open(file_server));
    // The Open's path is gone by now, so there is nothing to report.
    EXPECT_TRUE(calls().refusals.empty());
    // The directory's slot was resumed: the next Open on it is started.
    auto const [other_client, other_server] = channel();
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, other_server);
    run();
    EXPECT_EQ(fs().opens.size(), 3u);
}

TEST_F(PendingTest, AnOpenCompletedWhenTheTableIsFullIsRefusedAndTheCookieReleased) {
    open_fs();
    fs().pend_open = true;
    auto const [file_client, file_server] = channel();
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, file_server);
    run();
    ASSERT_EQ(fs().opens.size(), 2u);
    // The table fills up while the Open waits: it needs its room at completion,
    // not when it began.
    EXPECT_EQ(free_connections(), 2u);

    EXPECT_EQ(
        server().complete(fs().opens[1].ticket, Fs::open_result(ustl::some(FS_DIRECTORY), "file")),
        Status::Ok);
    EXPECT_EQ(epitaph(file_client), Status::NoResource);
    EXPECT_FALSE(fake::kernel().is_open(file_server));
    EXPECT_EQ(fs().releases, std::vector<u64>{FS_FILE});
}

TEST_F(PendingTest, ClientThatClosedBeforeItsOpenCompletedIsReleasedOnceAtItsFirstTurn) {
    open_fs();
    fs().pend_open = true;
    auto const [file_client, file_server] = channel();
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, file_server);
    run();
    EXPECT_EQ(ours::handle_close(file_client), Status::Ok);

    EXPECT_EQ(
        server().complete(fs().opens[1].ticket, Fs::open_result(ustl::some(FS_DIRECTORY), "file")),
        Status::Ok);
    run();
    EXPECT_FALSE(fake::kernel().is_open(file_server));
    EXPECT_EQ(fs().releases, std::vector<u64>{FS_FILE});
}

// --- a connection that is read while it waits -----------------------------------------

// Nothing in this library reads a suspended slot: these two resume the slot of
// a connection that waits on its own, as a program that broke the rule would, and
// check that nothing the Server held for the request is lost.

TEST_F(PendingTest, AMessageForAPendingConnectionDropsItAndClosesTheOpensServerEnd) {
    open_fs();
    fs().pend_open = true;
    auto const [file_client, file_server] = channel();
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, file_server);
    run();
    ASSERT_EQ(fs().opens.size(), 2u);
    EXPECT_TRUE(fake::kernel().is_open(file_server));

    EXPECT_EQ(event_loop().resume(loop::Key{DIRECTORY_SLOT, 0}), Status::Ok);
    request_dirents(m_dir_client, 3);
    run();

    // The directory connection went, and with it the server end it held.
    EXPECT_FALSE(fake::kernel().is_open(m_dir_server));
    EXPECT_FALSE(fake::kernel().is_open(file_server));
    EXPECT_EQ(epitaph(file_client), Status::PeerClosed);
    EXPECT_EQ(fs().releases, std::vector<u64>{FS_DIRECTORY});
}

TEST_F(PendingTest, ConnectionReleasedWhileItsOpenIsPendingClosesTheServerEndTheServerHeld) {
    open_fs();
    fs().pend_open = true;
    auto const [file_client, file_server] = channel();
    open(m_dir_client, "file", Flags::ProtocolFile | Flags::PermReadBytes, file_server);
    run();
    ASSERT_EQ(fs().opens.size(), 2u);

    EXPECT_EQ(ours::handle_close(m_dir_client), Status::Ok);
    EXPECT_EQ(event_loop().resume(loop::Key{DIRECTORY_SLOT, 0}), Status::Ok);
    run();

    EXPECT_FALSE(fake::kernel().is_open(m_dir_server));
    EXPECT_FALSE(fake::kernel().is_open(file_server));
    EXPECT_EQ(fs().releases, std::vector<u64>{FS_DIRECTORY});
}
} // namespace
