/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// block_server::BlockServer on a real Loop, against the shared fake kernel:
// the slot model. A Read or a Write parks in a slot and hands the driver
// start(); the test plays the hardware's completion interrupt, calling
// finish() -- the driver's pass-through to complete() -- and reads the reply
// off the parked channel. What the cases pin: the inline GetInfo, the slot
// fields start() sees, the completion's data on a Read and its status-only
// form on a Write, the ShouldWait backpressure at a full tag depth, the
// client that goes away while parked, the client that reconnects on the same
// entry before a parked completion lands, a start() the hardware refuses,
// and the two close paths.
#include <block_server/block_server.hpp>

#include <algorithm>
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

constexpr auto BLOCK = std::string_view{"ours.block.Block"};

/// The decided sizes, used as numbers: a constant changed by mistake fails
/// the tests instead of moving them.
constexpr auto MAX_CLIENTS = usize{2};
constexpr auto MAX_INFLIGHT = usize{2};

/// A block's size and a Read's extent, the tests' own numbers.
constexpr auto BLOCK_SIZE = usize{512};
constexpr auto READ_BLOCKS = usize{2};

/// The test's driver: a geometry, and a start() that records what it was
/// asked to submit. Its finish() is the completion interrupt, a pass-through
/// to the library's complete().
class FakeDevice final: public block_server::BlockServer<FakeDevice, MAX_CLIENTS, MAX_INFLIGHT> {
  public:
    using Base = block_server::BlockServer<FakeDevice, MAX_CLIENTS, MAX_INFLIGHT>;
    using Storage = typename Base::Storage;

    struct Started {
        usize slot = 0;
        bool is_write = false;
        u64 lba = 0;
        u32 count = 0;
        std::vector<u8> data = {};
    };

    std::vector<Started> started;
    Status start_status = Status::Ok;

    explicit FakeDevice(Storage &storage, loop::Loop &loop) : Base(storage, loop) {}

    [[nodiscard]]
    auto geometry() const -> block_server::Geometry {
        return {.block_size = BLOCK_SIZE, .blocks = 64};
    }

    auto start(usize const index, std::span<u8 const> const data) -> Status {
        auto const &parked = slot(index);
        started.push_back({.slot = index,
                           .is_write = !data.empty(),
                           .lba = parked.lba,
                           .count = parked.count,
                           .data = {data.begin(), data.end()}});
        return start_status;
    }

    /// The completion interrupt, played by the test.
    auto finish(usize const index, Status const status, std::span<u8 const> const data = {})
        -> void {
        complete(index, status, data);
    }
};

/// A fresh fake kernel, a Loop on it, and a FakeDevice served as
/// ours.block.Block behind an outgoing directory -- the wiring a driver's
/// main() builds, minus the hardware. The storages are on the heap: together
/// they are far past what a stack frame carries.
class BlockServerTest: public testing::Test {
  protected:
    BlockServerTest() {
        fake::kernel().reset();
        auto port = Uandle::Invalid;
        EXPECT_EQ(ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok);
        m_slots = std::make_unique<std::array<loop::Slot, 40>>();
        m_storage = std::make_unique<FakeDevice::Storage>();
        m_out_storage = std::make_unique<ioservice::Outgoing::Storage>();
        m_loop.emplace(port, *m_slots);
        m_device = std::make_unique<FakeDevice>(*m_storage, *m_loop);
        m_outgoing.emplace(*m_out_storage, *m_loop);
        EXPECT_EQ(m_outgoing->add_protocol(BLOCK, *m_device), Status::Ok);
        auto const ends = channel();
        EXPECT_EQ(m_outgoing->serve(ends[1]), Status::Ok);
        m_directory = ends[0];
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

    /// A client of ours.block.Block: opens it through the outgoing
    /// directory, the way the root forwards an Open -- the Open's object is
    /// the pair's server end, which the handler takes -- and returns the
    /// client end, on which the test writes requests and reads replies.
    auto connect_block() -> Uandle {
        auto const ends = channel();
        std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
        std::array<Uandle, 1> handles{};
        auto const encoded = ours_io::encode_open(bytes, handles,
                                                  {.flags = ours_io::Flags::ProtocolService,
                                                   .path = "svc/ours.block.Block",
                                                   .object = ends[1]});
        EXPECT_TRUE(encoded.is_ok());
        auto const message = encoded.unwrap();
        EXPECT_EQ(ours::channel_write(m_directory, ours::ChannelWriteOptions::None,
                                      message.bytes.data(), static_cast<u32>(message.bytes.size()),
                                      message.handles.data(),
                                      static_cast<u32>(message.handles.size())),
                  Status::Ok);
        run();
        return ends[0];
    }

    /// Writes |encode|'s message on |client|.
    template <typename Encode>
    auto send(Uandle const client, Encode const encode) -> void {
        static auto BYTES = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto const encoded = encode(std::span{BYTES});
        ASSERT_TRUE(encoded.is_ok());
        auto const message = encoded.unwrap();
        EXPECT_EQ(ours::channel_write(client, ours::ChannelWriteOptions::None, message.bytes.data(),
                                      static_cast<u32>(message.bytes.size()),
                                      message.handles.data(),
                                      static_cast<u32>(message.handles.size())),
                  Status::Ok);
    }

    /// The bytes of the one message |client| holds, if it holds one, copied
    /// out: block replies carry no handles, so the bytes are the whole
    /// message, and each call's copy is its own.
    static auto read_message(Uandle const client) -> std::optional<std::vector<u8>> {
        static auto BYTES = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        auto const read =
            ours::channel_read(client, ours::ChannelReadOptions::None, BYTES.data(), nullptr,
                               static_cast<u32>(BYTES.size()), 0, &num_bytes, &num_handles);
        if (read != Status::Ok) {
            return std::nullopt;
        }
        return std::vector<u8>(BYTES.begin(), BYTES.begin() + num_bytes);
    }

    /// The decoded message: |bytes| as a wire::Message with no handles.
    static auto as_message(std::vector<u8> const &bytes) -> wire::Message {
        return {.bytes = bytes, .handles = {}};
    }

    std::unique_ptr<std::array<loop::Slot, 40>> m_slots;
    std::unique_ptr<FakeDevice::Storage> m_storage;
    std::unique_ptr<ioservice::Outgoing::Storage> m_out_storage;
    std::optional<loop::Loop> m_loop;
    std::optional<ioservice::Outgoing> m_outgoing;
    std::unique_ptr<FakeDevice> m_device;
    Uandle m_directory = Uandle::Invalid;
};

TEST_F(BlockServerTest, GetInfoAnswersFromTheGeometry) {
    auto const client = connect_block();
    send(client,
         [](std::span<u8> bytes) { return ours_block::encode_get_info(bytes, {.txid = 7}); });
    run();

    auto const message = read_message(client);
    ASSERT_TRUE(message.has_value());
    auto const answered = ours_block::decode_get_info_reply(as_message(*message), 7);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::Ok);
    EXPECT_EQ(answered->block_size, u32{BLOCK_SIZE});
    EXPECT_EQ(answered->blocks, u64{64});
    EXPECT_EQ(m_device->in_flight(), usize{0});
}

TEST_F(BlockServerTest, ReadParksInASlotAndCompletesWithTheData) {
    auto const client = connect_block();
    send(client, [](std::span<u8> bytes) {
        return ours_block::encode_read(bytes, {.txid = 9, .lba = 100, .count = READ_BLOCKS});
    });
    run();

    // Parked, not answered: the hardware is working on it.
    ASSERT_EQ(m_device->started.size(), usize{1});
    EXPECT_EQ(m_device->started[0].slot, usize{0});
    EXPECT_FALSE(m_device->started[0].is_write);
    EXPECT_EQ(m_device->started[0].lba, u64{100});
    EXPECT_EQ(m_device->started[0].count, u32{READ_BLOCKS});
    EXPECT_EQ(m_device->in_flight(), usize{1});

    // The completion: the data may view the driver's DMA mapping.
    auto const data = std::array<u8, READ_BLOCKS * BLOCK_SIZE>{1, 2, 3, 4};
    m_device->finish(0, Status::Ok, data);

    auto const message = read_message(client);
    ASSERT_TRUE(message.has_value());
    auto const answered = ours_block::decode_read_reply(as_message(*message), 9);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::Ok);
    ASSERT_EQ(answered->data.size(), usize{data.size()});
    EXPECT_TRUE(std::equal(data.begin(), data.end(), answered->data.begin()));
    EXPECT_EQ(m_device->in_flight(), usize{0});
}

TEST_F(BlockServerTest, WriteHandsItsBlocksToStart) {
    auto const client = connect_block();
    auto const blocks = std::array<u8, BLOCK_SIZE>{'x'};
    send(client, [&](std::span<u8> bytes) {
        return ours_block::encode_write(bytes, {.txid = 11, .lba = 5, .count = 1, .data = blocks});
    });
    run();

    ASSERT_EQ(m_device->started.size(), usize{1});
    EXPECT_TRUE(m_device->started[0].is_write);
    EXPECT_EQ(m_device->started[0].lba, u64{5});
    ASSERT_EQ(m_device->started[0].data.size(), usize{blocks.size()});
    EXPECT_EQ(m_device->started[0].data[0], 'x');

    m_device->finish(0, Status::Ok);
    auto const message = read_message(client);
    ASSERT_TRUE(message.has_value());
    auto const answered = ours_block::decode_write_reply(as_message(*message), 11);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::Ok);
    EXPECT_EQ(m_device->in_flight(), usize{0});
}

TEST_F(BlockServerTest, AFullDepthAnswersShouldWait) {
    auto const first = connect_block();
    auto const second = connect_block();
    send(first, [](std::span<u8> bytes) {
        return ours_block::encode_read(bytes, {.txid = 1, .lba = 10, .count = 1});
    });
    send(second, [](std::span<u8> bytes) {
        return ours_block::encode_read(bytes, {.txid = 2, .lba = 20, .count = 1});
    });
    run();
    ASSERT_EQ(m_device->in_flight(), usize{MAX_INFLIGHT});

    // The depth is full: the next request is answered ShouldWait, in its own
    // reply form, and the connection stays.
    auto const blocks = std::array<u8, BLOCK_SIZE>{};
    send(first, [&](std::span<u8> bytes) {
        return ours_block::encode_write(bytes, {.txid = 3, .lba = 30, .count = 1, .data = blocks});
    });
    run();
    auto const refused = read_message(first);
    ASSERT_TRUE(refused.has_value());
    auto const refused_status = ours_block::decode_write_reply(as_message(*refused), 3);
    ASSERT_TRUE(refused_status.is_ok());
    EXPECT_EQ(refused_status->status, Status::ShouldWait);
    EXPECT_EQ(m_device->in_flight(), usize{MAX_INFLIGHT});

    // The completions drain the table, each to its own client. An Ok
    // completion of a Read carries exactly its request's blocks.
    auto const one = std::array<u8, BLOCK_SIZE>{'a'};
    auto const two = std::array<u8, BLOCK_SIZE>{'b'};
    m_device->finish(0, Status::Ok, one);
    m_device->finish(1, Status::Ok, two);
    auto const first_reply = read_message(first);
    auto const second_reply = read_message(second);
    ASSERT_TRUE(first_reply.has_value());
    ASSERT_TRUE(second_reply.has_value());
    auto const first_answer = ours_block::decode_read_reply(as_message(*first_reply), 1);
    auto const second_answer = ours_block::decode_read_reply(as_message(*second_reply), 2);
    ASSERT_TRUE(first_answer.is_ok());
    ASSERT_TRUE(second_answer.is_ok());
    EXPECT_EQ(first_answer->status, Status::Ok);
    EXPECT_EQ(second_answer->status, Status::Ok);
    EXPECT_EQ(m_device->in_flight(), usize{0});
}

// The cross-wiring interleaving the mark-dead fix pins: client 1 parks and
// exits; the completion's reply fails, and the entry is marked dead -- NOT
// freed, because its slot is still armed on the dead channel. Client 2
// connects (a different entry, or the same one only after the stale packet
// retires it), parks, and gets ITS OWN reply -- never client 1's drop, and
// never destroyed by the stale packet that fired client 1's slot.
TEST_F(BlockServerTest, ADeadClientLeaveOthersConnectionsAlone) {
    auto const gone = connect_block();
    send(gone, [](std::span<u8> bytes) {
        return ours_block::encode_read(bytes, {.txid = 40, .lba = 1, .count = 1});
    });
    run();
    ASSERT_EQ(m_device->in_flight(), usize{1});

    auto _ = ours::handle_close(gone);
    run();
    m_device->finish(0, Status::Ok, std::array<u8, BLOCK_SIZE>{});
    EXPECT_EQ(m_device->in_flight(), usize{0});

    // Client 2 parks and is answered with its own completion, untouched by
    // the stale packet that fires client 1's slot.
    auto const second = connect_block();
    send(second, [](std::span<u8> bytes) {
        return ours_block::encode_read(bytes, {.txid = 41, .lba = 2, .count = 1});
    });
    run();
    ASSERT_EQ(m_device->started.size(), usize{2});
    m_device->finish(m_device->started.back().slot, Status::Ok, std::array<u8, BLOCK_SIZE>{'b'});
    auto const message = read_message(second);
    ASSERT_TRUE(message.has_value());
    auto const answered = ours_block::decode_read_reply(as_message(*message), 41);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::Ok);

    // And client 1's slot retired without serving anything of client 2's:
    // exactly one message was queued for client 2, and it is gone.
    EXPECT_FALSE(read_message(second).has_value());
}

TEST_F(BlockServerTest, AClientGoneWhileParkedDropsItsReply) {
    auto const client = connect_block();
    send(client, [](std::span<u8> bytes) {
        return ours_block::encode_read(bytes, {.txid = 5, .lba = 1, .count = 1});
    });
    run();
    ASSERT_EQ(m_device->in_flight(), usize{1});

    // The client goes away while the disk works: the pump frees the entry,
    // and the Loop closes the connected channel.
    auto _ = ours::handle_close(client);
    run();
    EXPECT_EQ(m_device->in_flight(), usize{1});

    // The completion finds the connection gone and drops the reply silently.
    auto const data = std::array<u8, BLOCK_SIZE>{'z'};
    m_device->finish(0, Status::Ok, data);
    EXPECT_EQ(m_device->in_flight(), usize{0});

    // And the server takes a new client as if nothing had happened.
    auto const again = connect_block();
    send(again,
         [](std::span<u8> bytes) { return ours_block::encode_get_info(bytes, {.txid = 6}); });
    run();
    auto const message = read_message(again);
    ASSERT_TRUE(message.has_value());
    auto const answered = ours_block::decode_get_info_reply(as_message(*message), 6);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::Ok);
}

TEST_F(BlockServerTest, ACompletionBeforeAReconnectedEntryIsDropped) {
    auto const client = connect_block();
    send(client, [](std::span<u8> bytes) {
        return ours_block::encode_read(bytes, {.txid = 8, .lba = 1, .count = 1});
    });
    run();
    ASSERT_EQ(m_device->in_flight(), usize{1});

    // The client goes away; a new one takes the freed entry, whose
    // generation has moved on. The fake kernel never reissues a handle
    // value, so this pins the generation half of the check: the parked
    // (channel, generation) pair no longer names any client entry.
    auto _ = ours::handle_close(client);
    run();
    auto const successor = connect_block();

    // The old completion is dropped: the successor's channel holds no reply.
    m_device->finish(0, Status::Ok, std::array<u8, BLOCK_SIZE>{});
    EXPECT_FALSE(read_message(successor).has_value());
    EXPECT_EQ(m_device->in_flight(), usize{0});

    // And the successor is served normally.
    send(successor,
         [](std::span<u8> bytes) { return ours_block::encode_get_info(bytes, {.txid = 21}); });
    run();
    auto const message = read_message(successor);
    ASSERT_TRUE(message.has_value());
    auto const answered = ours_block::decode_get_info_reply(as_message(*message), 21);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::Ok);
}

TEST_F(BlockServerTest, ARefusedStartAnswersTheStatusInline) {
    auto const client = connect_block();
    m_device->start_status = Status::BadState;
    send(client, [](std::span<u8> bytes) {
        return ours_block::encode_read(bytes, {.txid = 12, .lba = 2, .count = 1});
    });
    run();

    auto const message = read_message(client);
    ASSERT_TRUE(message.has_value());
    auto const answered = ours_block::decode_read_reply(as_message(*message), 12);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::BadState);
    EXPECT_EQ(m_device->in_flight(), usize{0});

    // The connection stays: the next request goes through.
    m_device->start_status = Status::Ok;
    send(client,
         [](std::span<u8> bytes) { return ours_block::encode_get_info(bytes, {.txid = 13}); });
    run();
    auto const info = read_message(client);
    ASSERT_TRUE(info.has_value());
    auto const answered_info = ours_block::decode_get_info_reply(as_message(*info), 13);
    ASSERT_TRUE(answered_info.is_ok());
    EXPECT_EQ(answered_info->status, Status::Ok);
}

TEST_F(BlockServerTest, AnUnknownOrdinalClosesWithoutAnEpitaph) {
    auto const client = connect_block();
    // A GetInfo whose ordinal is patched to one this server does not speak.
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const encoded = ours_block::encode_get_info(bytes, {.txid = 14});
    ASSERT_TRUE(encoded.is_ok());
    auto patched = bytes;
    patched[8] = 0xEF;
    patched[9] = 0xBE;
    patched[10] = 0xAD;
    patched[11] = 0xDE;
    auto const message = encoded.unwrap();
    EXPECT_EQ(ours::channel_write(client, ours::ChannelWriteOptions::None, patched.data(),
                                  static_cast<u32>(message.bytes.size()), nullptr, 0),
              Status::Ok);
    run();

    // The connection is gone, without an epitaph: nothing was written on the
    // client's channel, whose peer has closed.
    EXPECT_EQ(m_device->in_flight(), usize{0});
    EXPECT_FALSE(read_message(client).has_value());
    auto probe = std::array<u8, 1>{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    EXPECT_EQ(ours::channel_read(client, ours::ChannelReadOptions::None, probe.data(), nullptr, 0,
                                 0, &num_bytes, &num_handles),
              Status::PeerClosed);
}

TEST_F(BlockServerTest, CloseAnswersThenEnds) {
    auto const client = connect_block();
    send(client, [](std::span<u8> bytes) { return ours_block::encode_close(bytes, {.txid = 15}); });
    run();

    auto const message = read_message(client);
    ASSERT_TRUE(message.has_value());
    auto const answered = ours_io::decode_close_reply(as_message(*message), 15);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::Ok);
}

} // namespace
