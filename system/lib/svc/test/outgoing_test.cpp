/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// svc::Outgoing on a real Loop, against the fake kernel of fake_kernel.hpp:
// what add_protocol() and serve() refuse, and what each kind of message on
// the outgoing directory comes to -- an Open of svc/<name> handed to that
// protocol's handler, an Open of a name it does not serve answered NotFound,
// a directory opened through it, a malformed message and a client gone.
#include <svc/outgoing.hpp>

#include <array>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

#include "fake_kernel.hpp"
#include <fuchsia_io/fuchsia_io.hpp>
#include <gtest/gtest.h>
#include <loop/loop.hpp>
#include <ours/port.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
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
using fuchsia_io::Flags;
using ours::Status;
using ours::Uandle;

constexpr auto ECHO = std::string_view{"ours.test.Echo"};
constexpr auto OTHER = std::string_view{"ours.test.Other"};

/// What one protocol's handler was handed, in order.
struct Calls {
    std::vector<Uandle> channels;
};

auto record(void *const context, Uandle const channel) -> void {
    static_cast<Calls *>(context)->channels.push_back(channel);
}

/// A fresh fake kernel, a Loop on it, and an Outgoing on the Loop, with its
/// storage, which is too large for the stack.
class OutgoingTest: public testing::Test {
  protected:
    OutgoingTest() {
        fake::kernel().reset();
        auto port = Uandle::Invalid;
        EXPECT_EQ(ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok);
        m_storage = std::make_unique<svc::Outgoing::Storage>();
        m_loop.emplace(port, m_slots);
        m_outgoing.emplace(*m_storage, *m_loop);
    }

    auto outgoing() -> svc::Outgoing & {
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
        auto const encoded = fuchsia_io::encode_open(
            bytes, handles,
            fuchsia_io::OpenRequest{.flags = flags, .path = path, .object = object});
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

  private:
    std::array<loop::Slot, 8> m_slots{};
    std::unique_ptr<svc::Outgoing::Storage> m_storage;
    std::optional<loop::Loop> m_loop;
    std::optional<svc::Outgoing> m_outgoing;
};

TEST_F(OutgoingTest, AddProtocolRefusals) {
    Calls calls;
    auto const handler = svc::Handler{.connect = record, .context = &calls};
    EXPECT_EQ(outgoing().add_protocol("", handler), Status::InvArgs);
    EXPECT_EQ(outgoing().add_protocol(".", handler), Status::InvArgs);
    EXPECT_EQ(outgoing().add_protocol("a/b", handler), Status::InvArgs);
    EXPECT_EQ(outgoing().add_protocol(ECHO, svc::Handler{}), Status::InvArgs);

    EXPECT_EQ(outgoing().add_protocol(ECHO, handler), Status::Ok);
    EXPECT_EQ(outgoing().add_protocol(ECHO, handler), Status::AlreadyExists);

    // MAX_PROTOCOLS in all, then no more.
    constexpr auto NAMES = std::array<std::string_view, svc::MAX_PROTOCOLS>{
        "p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8",
    };
    for (auto index = usize{1}; index < svc::MAX_PROTOCOLS; ++index) {
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
    ASSERT_EQ(outgoing().add_protocol(ECHO, {.connect = record, .context = &echo_calls}),
              Status::Ok);
    ASSERT_EQ(outgoing().add_protocol(OTHER, {.connect = record, .context = &other_calls}),
              Status::Ok);
    auto const directory = serve();

    // What component_manager writes when it forwards a route into a provider
    // (OutgoingConnector::send): svc/<name>, as a service.
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
    ASSERT_EQ(outgoing().add_protocol(ECHO, {.connect = record, .context = &calls}), Status::Ok);
    auto const [client, server] = channel();
    open(directory, "svc/ours.test.Echo", Flags::ProtocolService, server);
    run();
    ASSERT_EQ(calls.channels.size(), 1u);
}

TEST_F(OutgoingTest, OpenOfAnUnservedNameAnswersNotFound) {
    Calls calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, {.connect = record, .context = &calls}), Status::Ok);
    auto const directory = serve();

    auto const [client, server] = channel();
    open(directory, "svc/ours.test.Missing", Flags::ProtocolService, server);
    run();

    EXPECT_TRUE(calls.channels.empty());
    EXPECT_EQ(epitaph(client), Status::NotFound);
    EXPECT_FALSE(fake::kernel().is_open(server));
    EXPECT_TRUE(fake::kernel().is_open(directory));
}

TEST_F(OutgoingTest, ProtocolsReachedThroughAnOpenedSvcDirectory) {
    Calls calls;
    ASSERT_EQ(outgoing().add_protocol(ECHO, {.connect = record, .context = &calls}), Status::Ok);
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
    for (auto round = usize{0}; round <= svc::MAX_CONNECTIONS; ++round) {
        auto const [client, server] = channel();
        ASSERT_EQ(outgoing().serve(server), Status::Ok);
        EXPECT_EQ(ours::handle_close(client), Status::Ok);
        run();
        EXPECT_FALSE(fake::kernel().is_open(server));
    }
}

TEST_F(OutgoingTest, ServeRefusesPastItsConnections) {
    std::vector<Uandle> clients;
    for (auto index = usize{0}; index < svc::MAX_CONNECTIONS; ++index) {
        clients.push_back(serve());
    }
    auto const [client, server] = channel();
    EXPECT_EQ(outgoing().serve(server), Status::NoResource);
    // Refused, so still the caller's.
    EXPECT_TRUE(fake::kernel().is_open(server));
}
} // namespace
