/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The LV block server on the fake kernel: two fake PV providers the pump
// plays -- one pattern per PV, each answered at whatever LBA the mapping
// names -- and the server over a two-PV group, served as ours.block.Block
// behind an outgoing directory, driven by a real client. What the cases
// pin: the geometry is the LV's own, a read across the linear seam comes
// back as the two PVs' patterns in order, a striped read takes the chunk
// round-robin's answers, a write lands on the PVs the mapping names (the
// providers record what they were asked to hold), and a PV that fails
// fails the request.
#include <volume/lv_server.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

#include <cstring>

#include "fake_kernel.hpp"
#include <gtest/gtest.h>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <prot/block.hpp>
#include <wire/wire.hpp>

namespace ours {
// NOLINTNEXTLINE(readability-identifier-naming): gtest's customisation point.
auto PrintTo(Status const status, std::ostream *const out) -> void {
    *out << to_string(status);
}
} // namespace ours

// The wait pumps the machine while it waits: the fake kernel's progress is
// this hook, which the fixture points at the PV providers' answering
// logic -- the LV server's PV calls park here, and the providers answer
// inside the pump.
static std::function<void()> WAIT_PUMP = [] {};

// The vDSO declaration carries the asm label; the label rides the
// declarator, not the trailing type.
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline,
                                  ours::Signals *observed) __asm__("ours_object_wait_one");
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline, ours::Signals *observed) {
    for (auto spin = u32{0}; spin < 10'000'000; ++spin) {
        WAIT_PUMP();
        // A queued message wins over the peer's death, as the block
        // client's other suites read it.
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

namespace {
using ours::Status;
using ours::Uandle;
using volume::EXTENT_SECTORS;
using volume::Header;
using volume::Kind;
using volume::Lv;
using volume::Segment;

constexpr auto SECTOR = usize{512};

/// One provider's answer: the byte a Read of its sector |index| carries.
constexpr auto pv_byte(u32 const pv, u64 const sector) -> u8 {
    return static_cast<u8>((pv * 100 + sector) % 251 + 1);
}

/// A marker type so the two FakePv constructions read as pairs.
struct uandle_pair {};

/// A fake PV provider: the server end of one channel pair, played in the
/// pump. It answers GetInfo with a fixed geometry, Read with its pattern,
/// and Write by recording the sectors it was asked to hold -- the test
/// reads them back through the same pattern slot.
class FakePv {
  public:
    FakePv(u32 const index, uandle_pair /*tag*/) : index_(index) {
        auto ends = std::array<Uandle, 2>{Uandle::Invalid, Uandle::Invalid};
        EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &ends[0], &ends[1]),
                  Status::Ok);
        client_end_ = ends[0];
        server_end_ = ends[1];
    }

    [[nodiscard]]
    auto client() const -> Uandle {
        return client_end_;
    }

    /// The next read or write answers this instead of its work, once, and
    /// the mode clears itself: the failure-injection the failure paths'
    /// tests need.
    Status fail_next_ = Status::Ok;

    /// Answers everything queued: the block protocol's frames, one at a
    /// time, per the patterns above.
    auto pump() -> void {
        if (!fake::kernel().is_open(server_end_) ||
            fake::kernel().queued_messages(server_end_) == 0) {
            return;
        }
        auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto handles = std::array<Uandle, 4>{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        EXPECT_EQ(ours::channel_read(server_end_, ours::ChannelReadOptions::None, bytes.data(),
                                     handles.data(), static_cast<u32>(bytes.size()),
                                     static_cast<u32>(handles.size()), &num_bytes, &num_handles),
                  Status::Ok);
        auto const header =
            wire::decode_header(std::span{bytes}.first(num_bytes), wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        if (header->ordinal == prot::block::GET_INFO.ordinal) {
            auto reply_bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
            auto const reply = prot::block::encode_get_info_reply(
                reply_bytes, header->txid,
                {.status = Status::Ok, .block_size = prot::block::BLOCK_SIZE, .blocks = 4096});
            ASSERT_TRUE(reply.is_ok());
            write_back(reply->bytes);
            return;
        }
        if (header->ordinal == prot::block::READ.ordinal) {
            auto const request = prot::block::decode_read(
                wire::Message{.bytes = std::span{bytes}.first(num_bytes), .handles = {}});
            ASSERT_TRUE(request.is_ok());
            if (fail_next_ != Status::Ok) {
                auto const injected = fail_next_;
                fail_next_ = Status::Ok;
                auto reply_bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
                auto const reply = prot::block::encode_read_reply(reply_bytes, header->txid,
                                                                  {.status = injected, .data = {}});
                ASSERT_TRUE(reply.is_ok());
                write_back(reply->bytes);
                return;
            }
            auto data = std::array<u8, prot::block::MAX_READ_BLOCKS * SECTOR>{};
            for (auto i = u32{0}; i < request->count; ++i) {
                for (auto b = usize{0}; b < SECTOR; ++b) {
                    data[usize{i} * SECTOR + b] = pv_byte(index_, request->lba + i);
                }
            }
            auto reply_bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
            auto const reply = prot::block::encode_read_reply(
                reply_bytes, header->txid,
                {.status = Status::Ok,
                 .data = std::span{data}.first(usize{request->count} * SECTOR)});
            ASSERT_TRUE(reply.is_ok());
            write_back(reply->bytes);
            return;
        }
        if (header->ordinal == prot::block::WRITE.ordinal) {
            auto const request = prot::block::decode_write(
                wire::Message{.bytes = std::span{bytes}.first(num_bytes), .handles = {}});
            ASSERT_TRUE(request.is_ok());
            if (fail_next_ != Status::Ok) {
                auto const injected = fail_next_;
                fail_next_ = Status::Ok;
                auto reply_bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
                auto const reply = prot::block::encode_write_reply(reply_bytes, header->txid,
                                                                   {.status = injected});
                ASSERT_TRUE(reply.is_ok());
                write_back(reply->bytes);
                return;
            }
            // The provider holds what it was asked to hold: each written
            // sector's first byte, remembered per sector -- the test
            // reads the write back through these.
            for (auto i = u32{0}; i < request->count; ++i) {
                held_[request->lba + i] = request->data[usize{i} * SECTOR];
            }
            auto reply_bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
            auto const reply =
                prot::block::encode_write_reply(reply_bytes, header->txid, {.status = Status::Ok});
            ASSERT_TRUE(reply.is_ok());
            write_back(reply->bytes);
            return;
        }
        FAIL() << "the PV provider got a frame it does not speak";
    }

    /// Whether the provider was asked to hold |sector| = |byte|, from a
    /// write the request's LV sector mapped here.
    [[nodiscard]]
    auto held(u64 const sector) const -> u8 {
        auto const it = held_.find(sector);
        return it == held_.end() ? pv_byte(index_, sector) : it->second;
    }

  private:
    auto write_back(std::span<u8 const> const bytes) -> void {
        EXPECT_EQ(ours::channel_write(server_end_, ours::ChannelWriteOptions::None, bytes.data(),
                                      static_cast<u32>(bytes.size()), nullptr, 0),
                  Status::Ok);
    }

    u32 index_ = 0;
    Uandle client_end_ = Uandle::Invalid;
    Uandle server_end_ = Uandle::Invalid;
    std::map<u64, u8> held_{};
};

/// A two-PV group of the tests' own: LV 0 linear (extent 0 on PV 0,
/// extent 1 on PV 1), LV 1 striped over both, chunk 8 sectors.
auto make_group() -> volume::VolumeGroup {
    auto linear = Lv{};
    std::memcpy(linear.name.data(), "lin", 3);
    linear.kind = Kind::Linear;
    linear.sector_count = 2 * u64{EXTENT_SECTORS};
    auto striped = Lv{};
    std::memcpy(striped.name.data(), "st", 2);
    striped.kind = Kind::Striped;
    striped.chunk_sectors = 8;
    striped.stripe_count = 2;
    striped.sector_count = 2 * u64{EXTENT_SECTORS};
    auto lvs = std::array<Lv, 2>{linear, striped};
    auto pv0 = std::array<Segment, 2>{Segment{.lv = 0,
                                              .pv = 0,
                                              .role = 0,
                                              .first_logical = 0,
                                              .first_physical = 0,
                                              .extent_count = 1},
                                      Segment{.lv = 1,
                                              .pv = 0,
                                              .role = static_cast<u16>(Kind::Striped),
                                              .first_logical = 0,
                                              .first_physical = 1,
                                              .extent_count = 1}};
    auto pv1 = std::array<Segment, 2>{Segment{.lv = 0,
                                              .pv = 1,
                                              .role = 0,
                                              .first_logical = 1,
                                              .first_physical = 3,
                                              .extent_count = 1},
                                      Segment{.lv = 1,
                                              .pv = 1,
                                              .role = static_cast<u16>(Kind::Striped),
                                              .first_logical = 1,
                                              .first_physical = 2,
                                              .extent_count = 1}};

    auto header0 = Header{};
    header0.vg_uuid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    header0.pv_index = 0;
    header0.pv_extent_count = 4;
    header0.lv_count = 2;
    header0.segment_count = 2;
    header0.lvs[0] = lvs[0];
    header0.lvs[1] = lvs[1];
    header0.segments[0] = pv0[0];
    header0.segments[1] = pv0[1];
    auto header1 = header0;
    header1.uuid[15] = 1; // the PV's own uuid, distinct from header0's
    header1.pv_index = 1;
    header1.segments[0] = pv1[0];
    header1.segments[1] = pv1[1];

    auto group = volume::VolumeGroup{};
    EXPECT_EQ(group.add_pv(header0), Status::Ok);
    EXPECT_EQ(group.add_pv(header1), Status::Ok);
    EXPECT_TRUE(group.is_complete());
    return group;
}

/// The harness: the fake kernel, the Loop, the two fake PVs, the LV
/// server over the group served as ours.block.Block, and a client on it.
class LvServerTest: public testing::Test {
  protected:
    LvServerTest() {
        fake::kernel().reset();
        auto port = Uandle::Invalid;
        EXPECT_EQ(ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok);
        slots_ = std::make_unique<std::array<loop::Slot, 40>>();
        out_storage_ = std::make_unique<ioservice::Outgoing::Storage>();
        server_storage_ = std::make_unique<Server::Storage>();
        loop_.emplace(port, *slots_);
        pvs_[0] = std::make_unique<FakePv>(0, uandle_pair{});
        pvs_[1] = std::make_unique<FakePv>(1, uandle_pair{});

        group_ = make_group();
        clients_[0] = std::make_unique<prot::block::Client>(pvs_[0]->client());
        clients_[1] = std::make_unique<prot::block::Client>(pvs_[1]->client());
        std::array<prot::block::Client *, 2> pv_clients{clients_[0].get(), clients_[1].get()};

        outgoing_.emplace(*out_storage_, *loop_);
        server_ = std::make_unique<Server>(*server_storage_, *loop_, group_,
                                           std::span<prot::block::Client *const>{pv_clients});
        server_->set_lv(0);
        EXPECT_EQ(outgoing_->add_protocol(prot::block::BLOCK_NAME, *server_), Status::Ok);
        auto ends = std::array<Uandle, 2>{Uandle::Invalid, Uandle::Invalid};
        EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &ends[0], &ends[1]),
                  Status::Ok);
        EXPECT_EQ(outgoing_->serve(ends[1]), Status::Ok);
        directory_ = ends[0];
        WAIT_PUMP = [this] {
            // The PV providers answer their channels, and the Loop
            // dispatches what the LV server parked: the client's request
            // reaches the server's start() here, whose own PV calls nest
            // one pump deeper and come back inside it.
            pvs_[0]->pump();
            pvs_[1]->pump();
            auto _ = loop_->run(0);
        };
    }

    ~LvServerTest() override {
        WAIT_PUMP = [] {};
    }

    struct TestLv final: volume::LvBlockServer<TestLv, 2> {
        using LvBlockServer::LvBlockServer;
    };
    using Server = TestLv;

    /// Opens the LV's protocol through the outgoing directory and returns
    /// the client end.
    auto connect_block() -> Uandle {
        auto ends = std::array<Uandle, 2>{Uandle::Invalid, Uandle::Invalid};
        EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &ends[0], &ends[1]),
                  Status::Ok);
        auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto handles = std::array<Uandle, 1>{};
        auto const encoded = prot::io::encode_open(bytes, handles,
                                                   {.flags = prot::io::Flags::ProtocolService,
                                                    .path = "svc/ours.block.Block",
                                                    .object = ends[1]});
        EXPECT_TRUE(encoded.is_ok());
        auto const message = encoded.unwrap();
        EXPECT_EQ(ours::channel_write(directory_, ours::ChannelWriteOptions::None,
                                      message.bytes.data(), static_cast<u32>(message.bytes.size()),
                                      message.handles.data(),
                                      static_cast<u32>(message.handles.size())),
                  Status::Ok);
        EXPECT_EQ(loop_->run(0), Status::TimeOut);
        return ends[0];
    }

    /// Dispatches everything queued.
    auto run() -> void {
        EXPECT_EQ(loop_->run(0), Status::TimeOut);
    }

    std::unique_ptr<std::array<loop::Slot, 40>> slots_;
    std::unique_ptr<ioservice::Outgoing::Storage> out_storage_;
    std::unique_ptr<Server::Storage> server_storage_;
    std::optional<loop::Loop> loop_;
    std::array<std::unique_ptr<FakePv>, 2> pvs_{};
    std::array<std::unique_ptr<prot::block::Client>, 2> clients_{};
    std::optional<ioservice::Outgoing> outgoing_;
    std::unique_ptr<Server> server_;
    volume::VolumeGroup group_{};
    Uandle directory_ = Uandle::Invalid;
};

TEST_F(LvServerTest, GeometryIsTheLvs) {
    auto const client = connect_block();
    auto control = prot::block::Client{client};
    auto const info = control.get_info();
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info->block_size, prot::block::BLOCK_SIZE);
    EXPECT_EQ(info->blocks, 2 * u64{EXTENT_SECTORS});
    run();
}

TEST_F(LvServerTest, ReadAcrossTheSeam) {
    auto const client = connect_block();
    auto control = prot::block::Client{client};
    // Three sectors from logical 2046: two on PV 0's extent 0, one on PV
    // 1's extent 3 -- the reply's first 1024 bytes are PV 0's pattern for
    // sectors 2046-2047, the last 512 PV 1's pattern for 6144.
    auto data = std::array<u8, 3 * SECTOR>{};
    auto const status = control.read(EXTENT_SECTORS - 2, 3, data);
    ASSERT_EQ(status, Status::Ok);
    for (auto i = usize{0}; i < 2 * SECTOR; ++i) {
        EXPECT_EQ(data[i], pv_byte(0, EXTENT_SECTORS - 2 + i / SECTOR)) << i;
    }
    for (auto i = usize{0}; i < SECTOR; ++i) {
        EXPECT_EQ(data[2 * SECTOR + i], pv_byte(1, 3 * u64{EXTENT_SECTORS})) << i;
    }
    run();
}

TEST_F(LvServerTest, StripedReadTakesTheRoundRobin) {
    auto control = prot::block::Client{connect_block()};
    server_->set_lv(1);
    // LV 1's sector 4 is chunk 0's second half? No: chunk 0 is sectors
    // 0-7, all on leg 0 -- sector 4 stays on PV 0, four sectors into the
    // leg. Sector 10 is chunk 1, leg 1.
    auto data = std::array<u8, 2 * SECTOR>{};
    ASSERT_EQ(control.read(4, 1, data), Status::Ok);
    EXPECT_EQ(data[0], pv_byte(0, u64{EXTENT_SECTORS} + 4));
    ASSERT_EQ(control.read(10, 1, data), Status::Ok);
    EXPECT_EQ(data[0], pv_byte(1, 2 * u64{EXTENT_SECTORS} + 2));
    run();
}

/// A PV that refuses: the run's status is the request's, and the volume
/// answers nothing on that sector range until the device answers again.
TEST_F(LvServerTest, FailingPvFailsTheRequest) {
    auto control = prot::block::Client{connect_block()};
    pvs_[1]->fail_next_ = Status::BadState;
    auto data = std::array<u8, 2 * SECTOR>{};
    // Logical sector 2048 is the linear seam's far side: PV 1's to answer,
    // and it refuses.
    EXPECT_EQ(control.read(EXTENT_SECTORS, 2, data), Status::BadState);
    // A write through the same refusal, and then the device answering
    // again: the request fails, the next one goes through.
    pvs_[1]->fail_next_ = Status::BadState;
    EXPECT_EQ(control.write(EXTENT_SECTORS, data), Status::BadState);
    EXPECT_EQ(control.read(EXTENT_SECTORS, 2, data), Status::Ok);
    run();
}

TEST_F(LvServerTest, WriteLandsOnTheMappedPvs) {
    auto control = prot::block::Client{connect_block()};
    // A write across the linear seam: sectors 2046-2048, the first two
    // mapped to PV 0, the last to PV 1's physical 6144.
    auto data = std::array<u8, 3 * SECTOR>{};
    for (auto i = usize{0}; i < data.size(); ++i) {
        data[i] = static_cast<u8>(0x40 + i % 200);
    }
    ASSERT_EQ(control.write(EXTENT_SECTORS - 2, data), Status::Ok);
    run();
    // The providers hold the write's bytes at the mapped physical
    // sectors, and nothing else changed there.
    EXPECT_EQ(pvs_[0]->held(EXTENT_SECTORS - 2), data[0]);
    EXPECT_EQ(pvs_[0]->held(EXTENT_SECTORS - 1), data[SECTOR]);
    EXPECT_EQ(pvs_[1]->held(3 * u64{EXTENT_SECTORS}), data[2 * SECTOR]);
}
} // namespace
