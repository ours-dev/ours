/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The device tree's core on a real Loop, against the shared fake kernel:
// the grant rule, the mount pool, the refusals, and the leaf forward. The
// test plays both sides but the registry's: a publisher whose outgoing
// records the channels a leaf's forward brings, and a client that walks the
// tree the way routing_checks does. What the cases pin: the proposal
// granted as proposed, the taken proposal answered with the class's lowest
// free number, classes naming separately, the mount pool's exhaustion
// answering NoResource with the directory handle consumed, a malformed
// Publish and an unknown ordinal each closing the connection, a leaf's
// open reaching its publisher as an Open of svc/<protocol>, an open of the
// tree's dev/ node under a path no use can name refused NotFound, and the
// client's granted-name span, filled and NUL-terminated, BufferTooSmall
// when it does not fit, a forward to a dead publisher reading PeerClosed
// with its report on the console, the subdirs dev/class and dev/class/block
// listing their own entries, the ninth dev/ bind refused with the epitaph,
// a publish past the tree's nodes refused NoResource with its mount slot
// back, a publisher gone before its answer still mounted and the registry
// serving on, and the client's own refusals -- a class past its bound
// answered before any I/O and a publish on a connection already gone
// reading PeerClosed.
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../registry.hpp"
#include "fake_kernel.hpp"
#include <gtest/gtest.h>
#include <loop/loop.hpp>
#include <prot/block.hpp>
#include <prot/dev_client.hpp>
#include <prot/io.hpp>
#include <rpc/rpc.hpp>
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

// The two calls the core's chain makes that the fake kernel does not model:
// the client's synchronous wait, answered here from the fake's queue (the
// test runs the Loop between steps, so a reply due is already queued), and
// ort::debug_line, over the fake's debug_write so the reports land in the
// same bucket the assertions can read.
// The wait pumps the machine while it waits: the fake kernel's progress is
// the Loop's dispatching, which the fixture lends. Empty before the fixture
// exists, where a wait would spin for ever anyway.
static std::function<void()> WAIT_PUMP = [] {};

// The vDSO declaration carries the asm label "ours_object_wait_one", whose
// definition on the host is this one: the same literal name, through the
// same label -- an extern "C" spelling would land a platform underscore the
// label's reference never carries, and the label rides the declarator, not
// the trailing type.
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline,
                                  ours::Signals *observed) __asm__("ours_object_wait_one");
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline, ours::Signals *observed) {
    for (auto spin = u32{0}; spin < 1'000'000; ++spin) {
        WAIT_PUMP();
        // A queued message wins over the peer's death: what the real kernel
        // asserts on a survivor holding a last message is both, and the
        // reader drains first. The epitaph case lives on this order.
        if (fake::kernel().queued_messages(handle) != 0 &&
            (signals & ours::Signals::Readable) != ours::Signals::None) {
            *observed = ours::Signals::Readable;
            return ours::Status::Ok;
        }
        if (fake::kernel().peer_closed(handle)) {
            *observed = ours::Signals::PeerClosed;
            return ours::Status::Ok;
        }
    }
    *observed = ours::Signals::None;
    return ours::Status::TimeOut;
}

auto ort::debug_line(std::initializer_list<std::string_view> pieces) -> ours::Status {
    auto text = std::string{};
    for (auto const piece: pieces) {
        text.append(piece);
    }
    text.push_back('\n');
    return ours::debug_write(text.data(), static_cast<u32>(text.size()));
}

auto ort::debug_line(std::string_view text) -> ours::Status {
    return ours::debug_write(text.data(), static_cast<u32>(text.size()));
}

namespace {
using ours::Status;
using ours::Uandle;

constexpr auto BLOCK = std::string_view{"ours.block.Block"};

/// The decided sizes, used as numbers: a constant changed by mistake fails
/// the tests instead of moving them.
constexpr auto MAX_MOUNTS = usize{8};
constexpr auto MAX_TREE_CONNECTIONS = usize{8};
constexpr auto TREE_NODES = usize{16};

/// A publisher whose outgoing records the channels leaf forwards bring: the
/// test's stand-in for a block driver, minus the hardware.
class RecordingHandler final: public ioservice::Handler {
  public:
    auto connect(Uandle const channel) -> void override {
        forwarded.push_back(channel);
    }

    std::vector<Uandle> forwarded;
};

/// A fresh fake kernel, a Loop on it, the registry wired as main() wires it
/// -- outgoing with the protocol and the dev/ subtree, the tree laid out --
/// and one publisher connected. The storages are on the heap: together they
/// are far past what a stack frame carries.
class RegistryTest: public testing::Test {
  protected:
    RegistryTest() {
        fake::kernel().reset();
        auto port = Uandle::Invalid;
        EXPECT_EQ(ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok);
        m_slots = std::make_unique<std::array<loop::Slot, 48>>();
        m_out_storage = std::make_unique<ioservice::Outgoing::Storage>();
        m_registry_storage = std::make_unique<device_registry::RegistryServer::Storage>();
        m_dev_storage = std::make_unique<device_registry::DevStorage>();
        m_loop.emplace(port, *m_slots);
        m_outgoing = std::make_unique<ioservice::Outgoing>(*m_out_storage, *m_loop);
        m_dev = std::make_unique<device_registry::DevSubtree>(*m_dev_storage, *m_loop, m_core);
        EXPECT_EQ(m_dev->build(), Status::Ok);
        m_registry = std::make_unique<device_registry::RegistryServer>(
            *m_registry_storage, *m_loop, m_core, m_dev->tree(), m_dev->class_node());
        EXPECT_EQ(m_outgoing->add_protocol(prot::dev::REGISTRY_NAME, *m_registry), Status::Ok);
        EXPECT_TRUE(m_outgoing->add_subtree("/dev", *m_dev).is_ok());
        auto const ends = channel();
        EXPECT_EQ(m_outgoing->serve(ends[1]), Status::Ok);
        m_directory = ends[0];
        WAIT_PUMP = [this] { (void)m_loop->run(0); };
    }

    /// Dispatches everything queued, and expects the port to end empty.
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

    /// An Open of |path| with |flags|, carrying |object|, written on
    /// |connection| -- how a bound connection's opens of the tree's subdirs
    /// and leaves are made, and, for the outgoing directory, how the root
    /// forwards an Open. The loop runs the routing.
    auto open_on(Uandle const connection, std::string_view const path, prot::io::Flags const flags,
                 Uandle const object) -> Status {
        auto handles = std::array{object};
        auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto const open =
            prot::io::encode_open(bytes, handles, {.flags = flags, .path = path, .object = object});
        if (open.is_err()) {
            return open.unwrap_err();
        }
        auto const message = wire::Message{.bytes = std::span{bytes}.first(open->bytes.size()),
                                           .handles = std::span{handles}.first(1)};
        return ours::channel_write(connection, ours::ChannelWriteOptions::None,
                                   message.bytes.data(), static_cast<u32>(message.bytes.size()),
                                   message.handles.data(), 1);
    }

    /// An Open of |path| with |flags|, carrying |object|, written on the
    /// outgoing directory -- how every connection from the namespace is made.
    auto open_on_directory(std::string_view const path, prot::io::Flags const flags,
                           Uandle const object) -> Status {
        return open_on(m_directory, path, flags, object);
    }

    /// The names one ReadDirents on |connection| lists, "." included: the
    /// request asks for as much as a reply carries, so one answer holds the
    /// whole listing. The client's wait pumps the Loop, which answers it.
    auto list_directory(Uandle const connection) -> std::vector<std::string> {
        auto request_bytes = std::array<u8, wire::HEADER_SIZE + sizeof(u64)>{};
        auto const request = prot::io::encode_read_dirents(
            request_bytes, {.txid = 1, .max_bytes = prot::io::MAX_BUF});
        EXPECT_TRUE(request.is_ok());
        if (request.is_err()) {
            return {};
        }
        auto reply_bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto reply_handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
        auto const answered =
            rpc::exchange(connection, request.unwrap(), reply_bytes, reply_handles);
        EXPECT_TRUE(answered.is_ok());
        if (answered.is_err()) {
            return {};
        }
        auto const reply = prot::io::decode_read_dirents_reply(*answered, 1);
        EXPECT_TRUE(reply.is_ok());
        if (reply.is_err()) {
            return {};
        }
        auto names = std::vector<std::string>{};
        auto reader = prot::io::DirentReader{reply->dirents};
        while (true) {
            auto const entry = reader.next();
            EXPECT_TRUE(entry.is_ok());
            if (entry.is_err()) {
                break;
            }
            if (entry.unwrap().is_none()) {
                break;
            }
            names.emplace_back(entry.unwrap()->name);
        }
        return names;
    }

    /// A publisher: its outgoing with the recording handler served on one
    /// end, the other end the mount handle a publish carries, and the served
    /// end recorded so a test can take the outgoing down. The handler is on
    /// the heap; the recording runs past the test step that made it.
    struct Publisher {
        std::unique_ptr<ioservice::Outgoing::Storage> storage;
        std::unique_ptr<RecordingHandler> handler;
        std::unique_ptr<ioservice::Outgoing> outgoing;
        Uandle mount = Uandle::Invalid;
        Uandle served = Uandle::Invalid;
    };

    auto make_publisher() -> Publisher {
        auto publisher = Publisher{};
        publisher.storage = std::make_unique<ioservice::Outgoing::Storage>();
        publisher.handler = std::make_unique<RecordingHandler>();
        publisher.outgoing = std::make_unique<ioservice::Outgoing>(*publisher.storage, *m_loop);
        EXPECT_EQ(publisher.outgoing->add_protocol(BLOCK, *publisher.handler), Status::Ok);
        auto const ends = channel();
        EXPECT_EQ(publisher.outgoing->serve(ends[1]), Status::Ok);
        publisher.mount = ends[0];
        publisher.served = ends[1];
        return publisher;
    }

    /// Connects to the registry the way a driver does -- Open of
    /// svc/ours.dev.Registry through the outgoing directory -- and returns
    /// the client end for a prot::dev::Client.
    auto connect_registry() -> Uandle {
        auto const ends = channel();
        EXPECT_EQ(
            open_on_directory("svc/ours.dev.Registry", prot::io::Flags::ProtocolService, ends[1]),
            Status::Ok);
        run();
        return ends[0];
    }

    /// Publishes through a fresh registry connection, returns the codec's
    /// status, and fills |granted| with the granted name.
    auto publish(std::string_view const kind, std::string_view const name,
                 std::string_view const protocol, Uandle const mount, std::span<char> granted)
        -> Status {
        auto client = prot::dev::Client{connect_registry()};
        return client.publish(kind, name, protocol, mount, granted);
    }

    /// The mounts, for the assertions.
    [[nodiscard]]
    auto mounts() -> std::array<device_registry::Mount, MAX_MOUNTS> & {
        return m_core.mounts();
    }

    std::unique_ptr<std::array<loop::Slot, 48>> m_slots;
    std::unique_ptr<ioservice::Outgoing::Storage> m_out_storage;
    std::unique_ptr<device_registry::RegistryServer::Storage> m_registry_storage;
    std::unique_ptr<device_registry::DevStorage> m_dev_storage;
    device_registry::Core m_core;
    std::optional<loop::Loop> m_loop;
    std::unique_ptr<ioservice::Outgoing> m_outgoing;
    std::unique_ptr<device_registry::DevSubtree> m_dev;
    std::unique_ptr<device_registry::RegistryServer> m_registry;
    Uandle m_directory = Uandle::Invalid;
};

} // namespace

/// The first publisher's proposal is granted as proposed.
TEST_F(RegistryTest, ProposalGrantedAsProposed) {
    auto publisher = make_publisher();
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(publish("block", "000", BLOCK, publisher.mount, granted), Status::Ok);
    EXPECT_STREQ(granted.data(), "000");
    run();
    ASSERT_EQ(mounts()[0].used, true);
    EXPECT_EQ(mounts()[0].kind, "block");
    EXPECT_EQ(mounts()[0].name, "000");
}

/// A taken proposal is answered with the class's lowest free number.
TEST_F(RegistryTest, TakenProposalGetsLowestFree) {
    auto first = make_publisher();
    auto second = make_publisher();
    auto third = make_publisher();
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(publish("block", "000", BLOCK, first.mount, granted), Status::Ok);
    EXPECT_STREQ(granted.data(), "000");
    EXPECT_EQ(publish("block", "000", BLOCK, second.mount, granted), Status::Ok);
    EXPECT_STREQ(granted.data(), "001");
    // The next proposal skips the taken 001 too.
    EXPECT_EQ(publish("block", "001", BLOCK, third.mount, granted), Status::Ok);
    EXPECT_STREQ(granted.data(), "002");
    run();
    EXPECT_EQ(mounts()[1].name, "001");
    EXPECT_EQ(mounts()[2].name, "002");
}

/// Classes name separately: 000 in block does not hold 000 in input.
TEST_F(RegistryTest, ClassesNameSeparately) {
    auto block = make_publisher();
    auto input = make_publisher();
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(publish("block", "000", BLOCK, block.mount, granted), Status::Ok);
    EXPECT_STREQ(granted.data(), "000");
    EXPECT_EQ(publish("input", "000", "ours.input.Device", input.mount, granted), Status::Ok);
    EXPECT_STREQ(granted.data(), "000");
    run();
    EXPECT_EQ(mounts()[1].kind, "input");
}

/// The mount pool's exhaustion answers NoResource, and the refused
/// publish's directory handle went with the message either way -- the
/// kernel consumes what a write carries whatever it answers.
TEST_F(RegistryTest, PoolExhaustionAnswersNoResource) {
    auto granted = std::array<char, 8>{};
    for (auto index = usize{0}; index < MAX_MOUNTS; ++index) {
        auto publisher = make_publisher();
        EXPECT_EQ(publish("block", "000", BLOCK, publisher.mount, granted), Status::Ok);
    }
    auto overflow = make_publisher();
    EXPECT_EQ(publish("block", "000", BLOCK, overflow.mount, granted), Status::NoResource);
}

/// A malformed Publish closes the connection: the client's next call reads
/// the peer gone.
TEST_F(RegistryTest, MalformedPublishClosesConnection) {
    auto const ends = channel();
    EXPECT_EQ(open_on_directory("svc/ours.dev.Registry", prot::io::Flags::ProtocolService, ends[1]),
              Status::Ok);
    run();
    // A Publish whose body claims more bytes than the message carries.
    auto handles = std::array<Uandle, 1>{ends[1]};
    auto bytes = std::array<u8, 64>{};
    auto const built = prot::dev::encode_publish(
        bytes, handles,
        {.txid = 1, .kind = "block", .name = "000", .protocol = BLOCK, .directory = ends[1]});
    ASSERT_TRUE(built.is_ok());
    auto message = wire::Message{.bytes = std::span{bytes}.first(built->bytes.size() - 1),
                                 .handles = std::span{handles}.first(1)};
    // The build stamped a fresh handle value into handles[0]; write the
    // truncated body with it, one byte short: the decoder refuses, the
    // connection ends, and the client reads PeerClosed.
    EXPECT_EQ(ours::channel_write(ends[0], ours::ChannelWriteOptions::None, message.bytes.data(),
                                  static_cast<u32>(message.bytes.size()), message.handles.data(),
                                  1),
              Status::Ok);
    run();
    auto client = prot::dev::Client{ends[0]};
    auto granted = std::array<char, 8>{};
    // A real handle for the directory: the codec's own refusal for Invalid
    // would fire before any byte reached the wire, and the point here is the
    // closed connection, not the codec.
    auto const spare = channel();
    EXPECT_EQ(client.publish("block", "000", BLOCK, spare[0], granted), Status::PeerClosed);
}

/// An unknown ordinal closes the connection, as a closed protocol's
/// contract states.
TEST_F(RegistryTest, UnknownOrdinalClosesConnection) {
    auto client_handle = connect_registry();
    auto bytes = std::array<u8, 24>{};
    auto const built = prot::block::encode_get_info(bytes, {.txid = 1});
    ASSERT_TRUE(built.is_ok());
    auto const message =
        wire::Message{.bytes = std::span{bytes}.first(built->bytes.size()), .handles = {}};
    EXPECT_EQ(ours::channel_write(client_handle, ours::ChannelWriteOptions::None,
                                  message.bytes.data(), static_cast<u32>(message.bytes.size()),
                                  nullptr, 0),
              Status::Ok);
    run();
    auto client = prot::dev::Client{client_handle};
    auto granted = std::array<char, 8>{};
    // A real handle, for the same reason as above: the codec's refusal would
    // fire first, and what is under test is the closed connection.
    auto const spare = channel();
    EXPECT_EQ(client.publish("block", "000", BLOCK, spare[0], granted), Status::PeerClosed);
}

/// A leaf's open reaches its publisher as an Open of svc/<protocol>: the
/// recorded channel is the proof, one per open.
TEST_F(RegistryTest, LeafOpenReachesPublisher) {
    auto publisher = make_publisher();
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(publish("block", "000", BLOCK, publisher.mount, granted), Status::Ok);
    run();

    // The namespace entry: an Open of dev/ itself, which binds the client to
    // the tree's root.
    auto const tree_ends = channel();
    // The namespace's Open carries the use's rights beside the directory
    // bit -- what init's forward writes -- and the rights are what the
    // bound connection's opens are checked against.
    EXPECT_EQ(open_on_directory("dev", prot::io::Flags::ProtocolDirectory | prot::io::PERM_READABLE,
                                tree_ends[1]),
              Status::Ok);
    run();

    // The leaf: an Open of class/block/000 relative to that root, as
    // fsio::connect spells it from the bound entry.
    auto const leaf_ends = channel();
    auto handles = std::array{leaf_ends[1]};
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto const open = prot::io::encode_open(bytes, handles,
                                            {.flags = prot::io::Flags::ProtocolService,
                                             .path = "class/block/000",
                                             .object = leaf_ends[1]});
    ASSERT_TRUE(open.is_ok());
    EXPECT_EQ(ours::channel_write(tree_ends[0], ours::ChannelWriteOptions::None, bytes.data(),
                                  static_cast<u32>(open->bytes.size()), handles.data(), 1),
              Status::Ok);
    run();
    ASSERT_EQ(publisher.handler->forwarded.size(), 1u);
}

/// An Open of dev/ under a path no use can name is refused NotFound, the
/// epitaph the opener reads.
TEST_F(RegistryTest, UnknownTreePathRefusedNotFound) {
    auto const ends = channel();
    EXPECT_EQ(open_on_directory("dev/class/input", prot::io::Flags::ProtocolDirectory, ends[1]),
              Status::Ok);
    run();
    // The subtree refused it: the server end is closed, the client end
    // carries the epitaph.
    auto observed = ours::Signals::None;
    EXPECT_EQ(ours::object_wait_one(ends[0], ours::Signals::Readable | ours::Signals::PeerClosed,
                                    ours::INSTANT_INFINITE, &observed),
              Status::Ok);
    EXPECT_FALSE(!(observed & ours::Signals::Readable));
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    EXPECT_EQ(ours::channel_read(ends[0], ours::ChannelReadOptions::None, bytes.data(),
                                 handles.data(), static_cast<u32>(bytes.size()),
                                 static_cast<u32>(handles.size()), &num_bytes, &num_handles),
              Status::Ok);
    auto const message = wire::Message{.bytes = std::span{bytes}.first(num_bytes), .handles = {}};
    auto const epitaph = wire::decode_epitaph(message);
    ASSERT_TRUE(epitaph.is_ok());
    EXPECT_EQ(epitaph.unwrap(), Status::NotFound);
}

/// The client's granted span: NUL-terminated on Ok, BufferTooSmall when the
/// name and its NUL do not fit.
TEST_F(RegistryTest, GrantedSpanFilledAndBounded) {
    auto publisher = make_publisher();
    auto tight = std::array<char, 3>{};
    EXPECT_EQ(publish("block", "000", BLOCK, publisher.mount, tight), Status::BufferTooSmall);
    auto fit = std::array<char, 4>{};
    auto second = make_publisher();
    EXPECT_EQ(publish("block", "000", BLOCK, second.mount, fit), Status::Ok);
    EXPECT_STREQ(fit.data(), "001");
}

/// A leaf's open forwarded to a publisher whose outgoing has died: the
/// write fails, the failure report runs, nothing reaches the publisher, and
/// the leaf-open's client end reads PeerClosed -- the Open's handle went
/// with the refused write, consumed by the kernel either way.
TEST_F(RegistryTest, ForwardToDeadPublisherReadsPeerClosed) {
    auto publisher = make_publisher();
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(publish("block", "000", BLOCK, publisher.mount, granted), Status::Ok);
    run();

    // The publisher's outgoing goes away: its serving end closes, and the
    // mount's directory end loses its peer.
    EXPECT_EQ(ours::handle_close(publisher.served), Status::Ok);

    // The walk to the leaf, as LeafOpenReachesPublisher makes it.
    auto const tree_ends = channel();
    EXPECT_EQ(open_on_directory("dev", prot::io::Flags::ProtocolDirectory | prot::io::PERM_READABLE,
                                tree_ends[1]),
              Status::Ok);
    run();
    auto const leaf_ends = channel();
    EXPECT_EQ(
        open_on(tree_ends[0], "class/block/000", prot::io::Flags::ProtocolService, leaf_ends[1]),
        Status::Ok);
    run();

    // Nothing arrived: the forward died on its write, and the report says
    // so on the debug console.
    ASSERT_TRUE(publisher.handler->forwarded.empty());
    auto reported = false;
    for (auto const &line: fake::kernel().debug_writes()) {
        if (line.find("cannot forward a leaf's Open") != std::string::npos) {
            reported = true;
        }
    }
    EXPECT_TRUE(reported);
    auto observed = ours::Signals::None;
    EXPECT_EQ(ours::object_wait_one(leaf_ends[0],
                                    ours::Signals::Readable | ours::Signals::PeerClosed,
                                    ours::INSTANT_INFINITE, &observed),
              Status::Ok);
    EXPECT_FALSE(!(observed & ours::Signals::PeerClosed));
}

/// The subdirs a use's namespace entry can name, bound like the root: each
/// lists its own entries through the bound connection -- class/ its class
/// directories, class/block/ the mounted leaves.
TEST_F(RegistryTest, SubdirBindsListTheirEntries) {
    auto publisher = make_publisher();
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(publish("block", "000", BLOCK, publisher.mount, granted), Status::Ok);
    run();

    auto const class_ends = channel();
    EXPECT_EQ(open_on_directory("dev/class",
                                prot::io::Flags::ProtocolDirectory | prot::io::PERM_READABLE,
                                class_ends[1]),
              Status::Ok);
    run();
    auto const listed = list_directory(class_ends[0]);
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0], ".");
    EXPECT_EQ(listed[1], "block");

    auto const block_ends = channel();
    EXPECT_EQ(open_on_directory("dev/class/block",
                                prot::io::Flags::ProtocolDirectory | prot::io::PERM_READABLE,
                                block_ends[1]),
              Status::Ok);
    run();
    auto const leaves = list_directory(block_ends[0]);
    ASSERT_EQ(leaves.size(), 2u);
    EXPECT_EQ(leaves[0], ".");
    EXPECT_EQ(leaves[1], "000");
}

/// The tree's connection table full: the ninth dev/ bind is refused, and
/// its client end reads the epitaph naming NoResource.
TEST_F(RegistryTest, TreeConnectionExhaustionRefusesTheNinth) {
    auto held = std::array<std::array<Uandle, 2>, MAX_TREE_CONNECTIONS + 1>{};
    for (auto index = usize{0}; index < MAX_TREE_CONNECTIONS + 1; ++index) {
        held[index] = channel();
        EXPECT_EQ(open_on_directory("dev",
                                    prot::io::Flags::ProtocolDirectory | prot::io::PERM_READABLE,
                                    held[index][1]),
                  Status::Ok);
    }
    run();
    // The first eight binds hold the table; the ninth's server end carries
    // the refusal's epitaph and went with it.
    auto const ninth = held[MAX_TREE_CONNECTIONS][0];
    auto observed = ours::Signals::None;
    EXPECT_EQ(ours::object_wait_one(ninth, ours::Signals::Readable | ours::Signals::PeerClosed,
                                    ours::INSTANT_INFINITE, &observed),
              Status::Ok);
    EXPECT_FALSE(!(observed & ours::Signals::Readable));
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    EXPECT_EQ(ours::channel_read(ninth, ours::ChannelReadOptions::None, bytes.data(),
                                 handles.data(), static_cast<u32>(bytes.size()),
                                 static_cast<u32>(handles.size()), &num_bytes, &num_handles),
              Status::Ok);
    auto const message = wire::Message{.bytes = std::span{bytes}.first(num_bytes), .handles = {}};
    auto const epitaph = wire::decode_epitaph(message);
    ASSERT_TRUE(epitaph.is_ok());
    EXPECT_EQ(epitaph.unwrap(), Status::NoResource);
}

/// The tree's nodes full: class directories and service nodes one pool,
/// three of them spent on the layout, two to every class a publish makes --
/// six classes fit, and every publish past them is refused NoResource, the
/// refused publish's mount slot coming back with the answer.
TEST_F(RegistryTest, TreeNodeExhaustionRefusesThePublish) {
    constexpr auto NEW_CLASSES = usize{(TREE_NODES - 3) / 2};
    auto publishers = std::array<Publisher, MAX_MOUNTS>{};
    auto granted = std::array<char, 8>{};
    for (auto index = usize{0}; index < MAX_MOUNTS; ++index) {
        publishers[index] = make_publisher();
        auto const kind = std::array<char, 2>{'c', static_cast<char>('0' + index)};
        auto const status = publish(std::string_view{kind.data(), kind.size()}, "000", "ours.x.Y",
                                    publishers[index].mount, granted);
        if (index < NEW_CLASSES) {
            EXPECT_EQ(status, Status::Ok);
        } else {
            EXPECT_EQ(status, Status::NoResource);
        }
    }
    // The two refusals spent no mount: the six grants hold their slots and
    // the last two stand free for the next publish, the tree's nodes being
    // the exhausted pool, not the mounts.
    run();
    EXPECT_FALSE(mounts()[6].used);
    EXPECT_FALSE(mounts()[7].used);

    // The refused classes' directories keep their names: a failed publish
    // recycles its mount slot, and a name that lived in the slot would
    // rename the node behind the next publish's back -- the phantom this
    // case exists to catch. The class directory lists c0..c5, each once.
    auto const tree_ends = channel();
    EXPECT_EQ(open_on_directory("dev/class",
                                prot::io::Flags::ProtocolDirectory | prot::io::PERM_READABLE,
                                tree_ends[1]),
              Status::Ok);
    run();
    auto const names = list_directory(tree_ends[0]);
    // Nine: ".", block, and c0..c6 -- c6's directory was created before its
    // service node was refused, and an empty class is harmless by design.
    EXPECT_EQ(names.size(), usize{NEW_CLASSES + 3});
    // The refused c6 and c7 name nothing: c6's directory keeps its own
    // name (no phantom rename from the recycled slot), and c7 never made a
    // directory at all.
    EXPECT_EQ(std::count(names.begin(), names.end(), "c6"), 1U);
    EXPECT_EQ(std::count(names.begin(), names.end(), "c7"), 0U);
}

/// A class past its bound is refused by the codec before any I/O: InvArgs,
/// and the connection lives on -- the same client's next publish goes
/// through.
TEST_F(RegistryTest, OverlongClassRefusedBeforeAnyIO) {
    auto publisher = make_publisher();
    auto const overlong = std::string(200, 'a');
    auto granted = std::array<char, 8>{};
    auto client = prot::dev::Client{connect_registry()};
    EXPECT_EQ(client.publish(overlong, "000", BLOCK, publisher.mount, granted), Status::InvArgs);
    EXPECT_EQ(client.publish("block", "000", BLOCK, publisher.mount, granted), Status::Ok);
    EXPECT_STREQ(granted.data(), "000");
}

/// A publish on a connection whose server end died before the registry ever
/// took it: the write reads PeerClosed and that is the publish's answer.
TEST_F(RegistryTest, PublishOnClosedConnectionReadsPeerClosed) {
    auto const ends = channel();
    // The Open is written but never dispatched, so the server end is still
    // the test's to close -- the connection it names never comes to be.
    EXPECT_EQ(open_on_directory("svc/ours.dev.Registry", prot::io::Flags::ProtocolService, ends[1]),
              Status::Ok);
    EXPECT_EQ(ours::handle_close(ends[1]), Status::Ok);
    auto client = prot::dev::Client{ends[0]};
    auto granted = std::array<char, 8>{};
    auto const spare = channel();
    EXPECT_EQ(client.publish("block", "000", BLOCK, spare[0], granted), Status::PeerClosed);
}

/// A publisher that closes its own end right after its Publish, before the
/// pump runs: the request still reads and the mount still happens -- the
/// mounted-from report says so, and the mount keeps the directory handle it
/// took -- and the answer's write then fails on the dead peer, which is
/// what ends the connection. The registry lives on: the next publish is
/// answered, the taken name behind it.
TEST_F(RegistryTest, PublishThenCloseEarlyStillMountsAndKeepsServing) {
    auto publisher = make_publisher();
    auto const ends = channel();
    EXPECT_EQ(open_on_directory("svc/ours.dev.Registry", prot::io::Flags::ProtocolService, ends[1]),
              Status::Ok);
    run();
    // The Publish, raw on the connection, as MalformedPublishClosesConnection
    // makes it: the codec's own encoder, a real directory handle riding.
    auto handles = std::array<Uandle, 1>{publisher.mount};
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto const built = prot::dev::encode_publish(bytes, handles,
                                                 {.txid = 1,
                                                  .kind = "block",
                                                  .name = "000",
                                                  .protocol = BLOCK,
                                                  .directory = publisher.mount});
    ASSERT_TRUE(built.is_ok());
    // The build stamped the fresh handle value into handles[0]; the write
    // carries it.
    EXPECT_EQ(ours::channel_write(ends[0], ours::ChannelWriteOptions::None, bytes.data(),
                                  static_cast<u32>(built->bytes.size()), handles.data(), 1),
              Status::Ok);
    // Gone before the pump runs: the answer has no reader to reach.
    EXPECT_EQ(ours::handle_close(ends[0]), Status::Ok);
    run();

    // The mount happened all the same, and keeps what it took.
    auto mounted = false;
    for (auto const &line: fake::kernel().debug_writes()) {
        if (line.find(" mounted from a publisher") != std::string::npos) {
            mounted = true;
        }
    }
    EXPECT_TRUE(mounted);
    ASSERT_EQ(mounts()[0].used, true);
    EXPECT_EQ(mounts()[0].kind, "block");
    EXPECT_EQ(mounts()[0].directory, publisher.mount);

    // The registry answered the next publish as if nothing had happened,
    // the taken proposal granted the class's next number.
    auto granted = std::array<char, 8>{};
    auto second = make_publisher();
    EXPECT_EQ(publish("block", "000", BLOCK, second.mount, granted), Status::Ok);
    EXPECT_STREQ(granted.data(), "001");
    run();
    ASSERT_EQ(mounts()[1].used, true);
}
