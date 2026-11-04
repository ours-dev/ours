/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// prot::block::Client against a fake Block provider: the typed client's
// calls and what each failure reads as. The test plays the provider inside
// the fixture's pump -- when the client's wait asks the machine to progress,
// the pump reads the request off the server end, decodes it with the codec's
// own decoders, and answers per the mode with the codec's reply encoders.
// What the cases pin: the geometry round trip, Read's pattern copied whole
// into the caller's buffer, Write's bytes arriving as sent and its Ok
// coming back, the refusals a client makes without consulting the provider
// (a buffer too small for the read, a Write that is not whole blocks), a
// reply whose data is shorter than the read asked reading BadState, a
// provider's failure status arriving as itself (GetInfo's among them), the
// epitaph a provider closes with standing in for the failure, the peer gone
// without one reading PeerClosed, a reply to another request's txid caught
// as InvArgs rather than hung on, a Read past the bound a message carries
// refused by the encoder before any message leaves, and ours.gpt's
// read_blocks shape: the round trip through it, its zero and over-bound
// counts refused, and its provider's failure surfacing.
#include <prot/block_client.hpp>

#include <array>
#include <functional>
#include <span>

#include "fake_kernel.hpp"
#include <gtest/gtest.h>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <wire/wire.hpp>

namespace ours {
// NOLINTNEXTLINE(readability-identifier-naming): gtest's customisation point.
auto PrintTo(Status const status, std::ostream *const out) -> void {
    *out << to_string(status);
}
} // namespace ours

// The wait pumps the machine while it waits: the fake kernel's progress is
// this hook, which the fixture points at the provider's answering logic.
static std::function<void()> WAIT_PUMP = [] {};

// The vDSO declaration carries the asm label; the label rides the
// declarator, not the trailing type (the extern "C" spelling would land a
// platform underscore the label's reference never carries).
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline,
                                  ours::Signals *observed) __asm__("ours_object_wait_one");
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline, ours::Signals *observed) {
    for (auto spin = u32{0}; spin < 1'000'000; ++spin) {
        WAIT_PUMP();
        // A queued message wins over the peer's death: a survivor holding a
        // last message reads it first, the epitaph case lives on this order.
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

/// The requests the round trips send, and the geometry the provider
/// answers with: the facts the test and the pump share.
constexpr auto LBA = u64{2048};
constexpr auto WRITE_LBA = u64{4096};
constexpr auto READ_BLOCKS = usize{2};
constexpr auto WRITE_BLOCKS = usize{3};
constexpr auto GEOMETRY_BLOCKS = u64{64};

/// The byte a Read's answer carries at |index|, and the byte a Write's data
/// carries at |index|: the one pattern both sides of the channel know.
constexpr auto pattern_byte(usize const index) -> u8 {
    return static_cast<u8>(index % 251 + 1);
}

/// One test's provider: the pump reads the request off the server end and
/// answers per the mode. GetInfo answers the geometry (GetInfoFailure the
/// status form instead); Read answers the whole read (ReadShort a block
/// short of it, ReadFailure the status form, WrongTxid a reply carrying
/// another request's txid); Write checks the blocks and answers Ok;
/// Epitaph closes with the epitaph after reading the request; Silent
/// closes without answering.
enum class Mode {
    GetInfo,
    GetInfoFailure,
    Read,
    ReadShort,
    ReadFailure,
    WrongTxid,
    Write,
    Epitaph,
    Silent,
    GetInfoSilent,
    WriteSilent,
    WriteGarbage,
    WriteWrongTxid,
};

/// A channel pair, the client end first.
auto channel() -> std::array<Uandle, 2> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &client, &server), Status::Ok);
    return {client, server};
}

/// The fixture: a fresh fake kernel, a pair, the pump pointed at the mode.
class OursBlockClient: public testing::Test {
  protected:
    OursBlockClient() {
        fake::kernel().reset();
        m_ends = channel();
        WAIT_PUMP = [this] { pump(); };
    }

    ~OursBlockClient() override {
        WAIT_PUMP = [] {};
    }

    /// One pump step: answer whatever is queued on the server end, per the
    /// mode. The client's calls spin this while they wait.
    auto pump() -> void {
        if (m_answered) {
            return;
        }
        if (fake::kernel().queued_messages(m_ends[1]) == 0) {
            return;
        }
        m_answered = true;
        auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        EXPECT_EQ(ours::channel_read(m_ends[1], ours::ChannelReadOptions::None, bytes.data(),
                                     handles.data(), static_cast<u32>(bytes.size()),
                                     static_cast<u32>(handles.size()), &num_bytes, &num_handles),
                  Status::Ok);
        auto const message = wire::Message{.bytes = std::span{bytes}.first(num_bytes),
                                           .handles = std::span{handles}.first(num_handles)};
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        auto const txid = header->txid;

        switch (m_mode) {
            case Mode::GetInfo: {
                auto const decoded = prot::block::decode_get_info(message);
                ASSERT_TRUE(decoded.is_ok());
                auto const reply =
                    prot::block::encode_get_info_reply(m_reply_scratch, txid,
                                                       {.status = Status::Ok,
                                                        .block_size = prot::block::BLOCK_SIZE,
                                                        .blocks = GEOMETRY_BLOCKS});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::GetInfoFailure: {
                // A provider that cannot say its geometry says so: the
                // status form, no numbers after it.
                auto const reply = prot::block::encode_get_info_reply(m_reply_scratch, txid,
                                                                      {.status = Status::NotFound});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::Read:
            case Mode::ReadShort:
            case Mode::ReadFailure:
            case Mode::WrongTxid: {
                auto const decoded = prot::block::decode_read(message);
                ASSERT_TRUE(decoded.is_ok());
                EXPECT_EQ(decoded->lba, LBA);
                EXPECT_EQ(decoded->count, u32{READ_BLOCKS});
                if (m_mode == Mode::ReadFailure) {
                    // A provider's failure: the status form, no data.
                    auto const reply = prot::block::encode_read_reply(m_reply_scratch, txid,
                                                                      {.status = Status::NotFound});
                    ASSERT_TRUE(reply.is_ok());
                    write_reply(*reply);
                    return;
                }
                auto reply_txid = txid;
                auto blocks = usize{decoded->count};
                if (m_mode == Mode::ReadShort) {
                    // One block less than the read asked: still whole
                    // blocks, so the provider's own encoder accepts it.
                    blocks -= 1;
                }
                if (m_mode == Mode::WrongTxid) {
                    // A reply to another request: the client matches on its
                    // own txid, and the mismatch is what it names.
                    reply_txid += 1;
                }
                auto const size = blocks * prot::block::BLOCK_SIZE;
                auto data =
                    std::array<u8, prot::block::MAX_READ_BLOCKS * prot::block::BLOCK_SIZE>{};
                for (auto index = usize{0}; index < size; ++index) {
                    data[index] = pattern_byte(index);
                }
                auto const reply = prot::block::encode_read_reply(
                    m_reply_scratch, reply_txid,
                    {.status = Status::Ok, .data = std::span{data}.first(size)});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::WriteWrongTxid:
            case Mode::Write: {
                auto const decoded = prot::block::decode_write(message);
                ASSERT_TRUE(decoded.is_ok());
                auto reply_txid = txid;
                if (m_mode == Mode::WriteWrongTxid) {
                    // The mismatch test writes the shared LBA, one block.
                    EXPECT_EQ(decoded->lba, LBA);
                    EXPECT_EQ(decoded->count, u32{1});
                    // A reply to another request: the mismatch is what the
                    // client names.
                    reply_txid += 1;
                } else {
                    EXPECT_EQ(decoded->lba, WRITE_LBA);
                    EXPECT_EQ(decoded->count, u32{WRITE_BLOCKS});
                }
                auto const reply = prot::block::encode_write_reply(m_reply_scratch, reply_txid,
                                                                   {.status = Status::Ok});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::Epitaph: {
                auto epitaph = wire::encode_epitaph(m_reply_scratch, Status::NotFound);
                ASSERT_TRUE(epitaph.is_ok());
                EXPECT_EQ(ours::channel_write(m_ends[1], ours::ChannelWriteOptions::None,
                                              epitaph->bytes.data(),
                                              static_cast<u32>(epitaph->bytes.size()), nullptr, 0),
                          Status::Ok);
                EXPECT_EQ(ours::handle_close(m_ends[1]), Status::Ok);
                return;
            }
            case Mode::Silent:
                // No answer: just the peer's death.
                EXPECT_EQ(ours::handle_close(m_ends[1]), Status::Ok);
                return;
            case Mode::GetInfoSilent:
            case Mode::WriteSilent:
                // No answer: the connection just dies.
                EXPECT_EQ(ours::handle_close(m_ends[1]), Status::Ok);
                return;
            case Mode::WriteGarbage: {
                // Bytes that are not a message: the header's magic is wrong.
                auto garbage = std::array<u8, 32>{};
                garbage.fill(0x5A);
                ASSERT_EQ(ours::channel_write(m_ends[1], ours::ChannelWriteOptions::None,
                                              garbage.data(), static_cast<u32>(garbage.size()),
                                              nullptr, 0),
                          Status::Ok);
                return;
            }
        }
    }

    /// Writes an encoded reply onto the server end, back to the client.
    auto write_reply(wire::Message const &reply) -> void {
        EXPECT_EQ(ours::channel_write(m_ends[1], ours::ChannelWriteOptions::None,
                                      reply.bytes.data(), static_cast<u32>(reply.bytes.size()),
                                      nullptr, 0),
                  Status::Ok);
    }

    std::array<Uandle, 2> m_ends{};
    Mode m_mode = Mode::GetInfo;
    bool m_answered = false;
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_reply_scratch{};
};

} // namespace

/// The geometry round trip: GetInfo answered 512/64, and the client's
/// Result carries the two numbers.
TEST_F(OursBlockClient, GetInfoRoundTrip) {
    auto client = prot::block::Client{m_ends[0]};
    auto const geometry = client.get_info();
    ASSERT_TRUE(geometry.is_ok());
    EXPECT_EQ(geometry->block_size, prot::block::BLOCK_SIZE);
    EXPECT_EQ(geometry->blocks, GEOMETRY_BLOCKS);
}

/// A provider's failure to the geometry arrives as itself, no numbers
/// after it.
TEST_F(OursBlockClient, GetInfoFailureArrivesAsItself) {
    m_mode = Mode::GetInfoFailure;
    auto client = prot::block::Client{m_ends[0]};
    EXPECT_EQ(client.get_info().unwrap_err(), Status::NotFound);
}

/// Read's round trip: the provider's pattern copied whole into the
/// caller's buffer.
TEST_F(OursBlockClient, ReadRoundTrip) {
    m_mode = Mode::Read;
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, READ_BLOCKS * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.read(LBA, u32{READ_BLOCKS}, out), Status::Ok);
    for (auto index = usize{0}; index < out.size(); ++index) {
        ASSERT_EQ(out[index], pattern_byte(index));
    }
}

/// A buffer too small for the read is refused before any message leaves:
/// the provider is not consulted.
TEST_F(OursBlockClient, ReadRefusesASmallBuffer) {
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, READ_BLOCKS * prot::block::BLOCK_SIZE - 1>{};
    EXPECT_EQ(client.read(LBA, u32{READ_BLOCKS}, out), Status::BufferTooSmall);
    EXPECT_EQ(fake::kernel().queued_messages(m_ends[1]), usize{0});
}

/// A read past the bound a message carries, with a buffer that would hold
/// it: the client's own size check passes, and the refusal is the encoder's
/// -- no message leaves for the provider to see.
constexpr auto PAST_BOUND_COUNT = u32{99};

TEST_F(OursBlockClient, ReadPastTheBoundRefusedByTheEncoder) {
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, PAST_BOUND_COUNT * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.read(LBA, PAST_BOUND_COUNT, out), Status::InvArgs);
    EXPECT_EQ(fake::kernel().queued_messages(m_ends[1]), usize{0});
}

/// Write's round trip: the blocks arrive as sent, and the provider's Ok
/// comes back as the write's.
TEST_F(OursBlockClient, WriteRoundTrip) {
    m_mode = Mode::Write;
    auto client = prot::block::Client{m_ends[0]};
    auto data = std::array<u8, WRITE_BLOCKS * prot::block::BLOCK_SIZE>{};
    for (auto index = usize{0}; index < data.size(); ++index) {
        data[index] = pattern_byte(index);
    }
    EXPECT_EQ(client.write(WRITE_LBA, data), Status::Ok);
}

/// The writes the client refuses itself, before any message leaves: an
/// empty write, one that is not whole blocks, and one past the bound a
/// message carries.
TEST_F(OursBlockClient, WriteRefusesBadShapes) {
    auto client = prot::block::Client{m_ends[0]};
    EXPECT_EQ(client.write(WRITE_LBA, std::span<u8 const>{}), Status::InvArgs);
    auto const ragged = std::array<u8, prot::block::BLOCK_SIZE + 1>{};
    EXPECT_EQ(client.write(WRITE_LBA, ragged), Status::InvArgs);
    auto const oversized =
        std::array<u8, (prot::block::MAX_READ_BLOCKS + 1) * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.write(WRITE_LBA, oversized), Status::InvArgs);
    EXPECT_EQ(fake::kernel().queued_messages(m_ends[1]), usize{0});
}

/// A reply whose data is shorter than the read asked: the client names the
/// answer BadState rather than copy a short read.
TEST_F(OursBlockClient, ShortReplyReadsBadState) {
    m_mode = Mode::ReadShort;
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, READ_BLOCKS * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.read(LBA, u32{READ_BLOCKS}, out), Status::BadState);
}

/// A provider's failure status arrives as itself.
TEST_F(OursBlockClient, FailureStatusArrivesAsItself) {
    m_mode = Mode::ReadFailure;
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, READ_BLOCKS * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.read(LBA, u32{READ_BLOCKS}, out), Status::NotFound);
}

/// The epitaph a provider closes with stands in for the failure.
TEST_F(OursBlockClient, EpitaphBecomesTheFailure) {
    m_mode = Mode::Epitaph;
    auto client = prot::block::Client{m_ends[0]};
    EXPECT_EQ(client.get_info().unwrap_err(), Status::NotFound);
}

/// A peer gone without an epitaph reads PeerClosed.
TEST_F(OursBlockClient, SilentCloseReadsPeerClosed) {
    m_mode = Mode::Silent;
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, READ_BLOCKS * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.read(LBA, u32{READ_BLOCKS}, out), Status::PeerClosed);
}

/// A reply to another request's txid: the mismatch is caught and named
/// InvArgs, not hung on.
TEST_F(OursBlockClient, WrongTxidReplyReadsInvArgs) {
    m_mode = Mode::WrongTxid;
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, READ_BLOCKS * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.read(LBA, u32{READ_BLOCKS}, out), Status::InvArgs);
}

/// ours.gpt's shape, the happy path: read_blocks carries the read whole,
/// the provider's pattern landing in the caller's buffer.
TEST_F(OursBlockClient, ReadBlocksRoundTrip) {
    m_mode = Mode::Read;
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, READ_BLOCKS * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.read_blocks(LBA, READ_BLOCKS, out), Status::Ok);
    for (auto index = usize{0}; index < out.size(); ++index) {
        ASSERT_EQ(out[index], pattern_byte(index));
    }
}

/// The counts read_blocks refuses itself, before any message leaves: zero,
/// and one past MAX_READ_BLOCKS.
TEST_F(OursBlockClient, ReadBlocksRefusesBadCounts) {
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, READ_BLOCKS * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.read_blocks(LBA, usize{0}, out), Status::InvArgs);
    EXPECT_EQ(client.read_blocks(LBA, prot::block::MAX_READ_BLOCKS + 1, out), Status::InvArgs);
    EXPECT_EQ(fake::kernel().queued_messages(m_ends[1]), usize{0});
}

/// A read_blocks whose provider answers a failure: the read's failure is
/// the caller's.
TEST_F(OursBlockClient, ReadBlocksSurfacesTheReadsFailure) {
    m_mode = Mode::ReadFailure;
    auto client = prot::block::Client{m_ends[0]};
    auto out = std::array<u8, READ_BLOCKS * prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.read_blocks(LBA, READ_BLOCKS, out), Status::NotFound);
}

/// GetInfo through a provider that dies without answering: the call's
/// failure is the client's.
TEST_F(OursBlockClient, GetInfoSilentReadsPeerClosed) {
    m_mode = Mode::GetInfoSilent;
    auto client = prot::block::Client{m_ends[0]};
    auto const geometry = client.get_info();
    EXPECT_EQ(geometry.unwrap_err(), Status::PeerClosed);
}

/// Write through a provider that dies without answering: PeerClosed.
TEST_F(OursBlockClient, WriteSilentReadsPeerClosed) {
    m_mode = Mode::WriteSilent;
    auto client = prot::block::Client{m_ends[0]};
    auto const data = std::array<u8, prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.write(LBA, data), Status::PeerClosed);
}

/// Write through a provider that answers garbage: the pipeline's header
/// check refuses it before the codec sees it, and the call's BadState is
/// the client's.
TEST_F(OursBlockClient, WriteGarbageReadsBadState) {
    m_mode = Mode::WriteGarbage;
    auto client = prot::block::Client{m_ends[0]};
    auto const data = std::array<u8, prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.write(LBA, data), Status::BadState);
}

/// Write answered with a reply to another request: the client matches on
/// its own txid, and the mismatch is the codec's InvArgs.
TEST_F(OursBlockClient, WriteWrongTxidReadsInvArgs) {
    m_mode = Mode::WriteWrongTxid;
    auto client = prot::block::Client{m_ends[0]};
    auto const data = std::array<u8, prot::block::BLOCK_SIZE>{};
    EXPECT_EQ(client.write(LBA, data), Status::InvArgs);
}
