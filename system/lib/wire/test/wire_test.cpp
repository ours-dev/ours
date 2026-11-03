/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The framing (<wire/wire.hpp>): the header every message starts with, the
// status form of a reply, the epitaph and handle fields. Each is written,
// read back, and broken byte by byte, since a server reads what programs it
// does not trust send it.
#include <wire/wire.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstdlib>

#include <gtest/gtest.h>

namespace {
using ours::Status;
using ours::Uandle;
using wire::Direction;
using wire::Header;
using wire::Message;
using wire::Method;
using wire::MethodKind;

// Every peer reads the header's fields at these offsets: the txid, the
// direction in byte 4, two reserved bytes, the magic and the ordinal.
static_assert(offsetof(Header, txid) == 0);
static_assert(offsetof(Header, direction) == 4);
static_assert(offsetof(Header, reserved) == 5);
static_assert(offsetof(Header, magic) == 7);
static_assert(offsetof(Header, ordinal) == 8);

/// Two made-up methods, written out whole so that their ordinals are patterns
/// that read at a glance in a hex dump. Their selectors are never hashed, so
/// these are not methods that wire::method() would make.
constexpr auto TWO_WAY = Method{
    .selector = "test/Protocol.TwoWay",
    .ordinal = 0x1234'5678'9abc'def0,
    .kind = MethodKind::TwoWay,
};
constexpr auto ONE_WAY = Method{
    .selector = "test/Protocol.OneWay",
    .ordinal = 0x0fed'cba9'8765'4321,
    .kind = MethodKind::OneWay,
};

constexpr auto FILL = u8{0xCC};

/// A buffer that starts full of FILL, so that a byte an encoder forgot to
/// write shows.
auto dirty_buffer() -> std::vector<u8> {
    return std::vector<u8>(wire::MAX_MESSAGE_BYTES, FILL);
}

/// A copy of |message|'s bytes, to be broken on purpose.
auto bytes_of(Message const message) -> std::vector<u8> {
    return {message.bytes.begin(), message.bytes.end()};
}

auto as_message(std::vector<u8> const &bytes, std::span<Uandle const> const handles = {})
    -> Message {
    return Message{.bytes = bytes, .handles = handles};
}

auto encoded_status_reply(std::vector<u8> &buffer, u32 const txid, Status const status) -> Message {
    auto encoded = wire::encode_status_reply(buffer, TWO_WAY, txid, status);
    EXPECT_TRUE(encoded.is_ok());
    return encoded.unwrap();
}

TEST(WireHeader, StartMessageZeroesAndWritesTheHeader) {
    auto buffer = dirty_buffer();
    auto started = wire::start_message(buffer, 40, 7, Direction::ToServer, TWO_WAY.ordinal);
    ASSERT_TRUE(started.is_ok());
    auto const bytes = started.unwrap();
    EXPECT_EQ(bytes.data(), buffer.data());
    ASSERT_EQ(bytes.size(), 40u);
    for (usize at = wire::HEADER_SIZE; at < bytes.size(); ++at) {
        EXPECT_EQ(bytes[at], 0) << "byte " << at;
    }
    // Only the message's own bytes are written.
    EXPECT_EQ(buffer[40], FILL);

    auto const header = wire::decode_header(bytes, Direction::ToServer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->txid, 7u);
    EXPECT_EQ(header->direction, Direction::ToServer);
    EXPECT_EQ(header->magic, wire::MAGIC);
    EXPECT_EQ(header->ordinal, TWO_WAY.ordinal);
    EXPECT_EQ(bytes[5], 0);
    EXPECT_EQ(bytes[6], 0);
}

TEST(WireHeader, StartMessageRefusals) {
    auto buffer = dirty_buffer();
    EXPECT_EQ(
        wire::start_message(buffer, wire::HEADER_SIZE - 1, 0, Direction::ToServer, 1).unwrap_err(),
        Status::InvArgs);
    EXPECT_EQ(wire::start_message(buffer, wire::MAX_MESSAGE_BYTES + 1, 0, Direction::ToServer, 1)
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(wire::start_message(std::span{buffer}.first(23), 24, 0, Direction::ToServer, 1)
                  .unwrap_err(),
              Status::BufferTooSmall);
}

TEST(WireHeader, DecodeRefusals) {
    auto buffer = dirty_buffer();
    auto const good = bytes_of(
        Message{.bytes = wire::start_message(buffer, 24, 0, Direction::ToServer, 1).unwrap(),
                .handles = {}});
    ASSERT_TRUE(wire::decode_header(good, Direction::ToServer).is_ok());

    // Shorter than a header, and longer than a channel message.
    EXPECT_TRUE(wire::decode_header(std::span{good}.first(15), Direction::ToServer).is_err());
    std::vector<u8> too_long(wire::MAX_MESSAGE_BYTES + 1, 0);
    std::copy(good.begin(), good.end(), too_long.begin());
    EXPECT_TRUE(wire::decode_header(too_long, Direction::ToServer).is_err());

    // Each of the bytes that must hold a fixed value.
    for (auto const at: {usize{5}, usize{6}, usize{7}}) {
        auto broken = good;
        broken[at] ^= 0x40;
        EXPECT_EQ(wire::decode_header(broken, Direction::ToServer).unwrap_err(), Status::InvArgs)
            << "byte " << at;
    }

    // A server never reads a reply as a request, nor a client the reverse.
    EXPECT_TRUE(wire::decode_header(good, Direction::ToClient).is_err());
    auto reply = good;
    reply[4] = std::to_underlying(Direction::ToClient);
    EXPECT_TRUE(wire::decode_header(reply, Direction::ToServer).is_err());
    auto unknown_direction = good;
    unknown_direction[4] = 2;
    EXPECT_TRUE(wire::decode_header(unknown_direction, Direction::ToServer).is_err());
    EXPECT_TRUE(wire::decode_header(unknown_direction, Direction::ToClient).is_err());
}

TEST(WireRequest, TxidRules) {
    auto buffer = dirty_buffer();

    // A one-way request carries txid 0, a two-way one 1 to MAX_TXID.
    EXPECT_TRUE(wire::start_request(buffer, 24, ONE_WAY, 0).is_ok());
    EXPECT_EQ(wire::start_request(buffer, 24, ONE_WAY, 1).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(wire::start_request(buffer, 24, TWO_WAY, 0).unwrap_err(), Status::InvArgs);
    EXPECT_TRUE(wire::start_request(buffer, 24, TWO_WAY, wire::MAX_TXID).is_ok());
    EXPECT_EQ(wire::start_request(buffer, 24, TWO_WAY, wire::MAX_TXID + 1).unwrap_err(),
              Status::InvArgs);

    auto const request = bytes_of(Message{
        .bytes = wire::start_request(buffer, 24, TWO_WAY, 9).unwrap(),
        .handles = {},
    });
    auto const header = wire::decode_request_header(as_message(request), TWO_WAY);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->txid, 9u);

    // The same bytes are not a request of another method.
    EXPECT_TRUE(wire::decode_request_header(as_message(request), ONE_WAY).is_err());

    // A two-way request with txid 0, or one above MAX_TXID.
    for (auto const txid: {u32{0}, wire::MAX_TXID + 1}) {
        auto broken = request;
        wire::store(std::span{broken}, 0, txid);
        EXPECT_TRUE(wire::decode_request_header(as_message(broken), TWO_WAY).is_err())
            << "txid " << txid;
    }
    // A one-way request with a txid.
    auto one_way = bytes_of(Message{
        .bytes = wire::start_request(buffer, 24, ONE_WAY, 0).unwrap(),
        .handles = {},
    });
    EXPECT_TRUE(wire::decode_request_header(as_message(one_way), ONE_WAY).is_ok());
    wire::store(std::span{one_way}, 0, u32{3});
    EXPECT_TRUE(wire::decode_request_header(as_message(one_way), ONE_WAY).is_err());
}

TEST(WireReply, StatusFormRoundTrip) {
    for (auto const status:
         {Status::Ok, Status::AccessDenied, Status::NotFound, Status::BufferTooSmall}) {
        auto buffer = dirty_buffer();
        auto const reply = encoded_status_reply(buffer, 42, status);
        ASSERT_EQ(reply.bytes.size(), wire::STATUS_FORM_SIZE);
        EXPECT_TRUE(reply.handles.empty());
        EXPECT_EQ(wire::load<u32>(reply.bytes, 20), 0u);

        auto const decoded = wire::decode_reply_status(reply, TWO_WAY, 42);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded.unwrap(), status);
    }
}

TEST(WireReply, StatusFormEncodeRefusals) {
    auto buffer = dirty_buffer();
    EXPECT_EQ(wire::encode_status_reply(buffer, ONE_WAY, 0, Status::Ok).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(wire::encode_status_reply(buffer, TWO_WAY, 0, Status::Ok).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(
        wire::encode_status_reply(buffer, TWO_WAY, wire::MAX_TXID + 1, Status::Ok).unwrap_err(),
        Status::InvArgs);
    EXPECT_EQ(wire::encode_status_reply(buffer, TWO_WAY, 1, Status::MaxCount).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(
        wire::encode_status_reply(std::span{buffer}.first(23), TWO_WAY, 1, Status::Ok).unwrap_err(),
        Status::BufferTooSmall);
}

TEST(WireReply, DecodeRefusals) {
    auto buffer = dirty_buffer();
    auto const good = bytes_of(encoded_status_reply(buffer, 42, Status::AccessDenied));
    ASSERT_TRUE(wire::decode_reply_status(as_message(good), TWO_WAY, 42).is_ok());

    // Another request's reply, another method's, or a one-way method's.
    EXPECT_TRUE(wire::decode_reply_status(as_message(good), TWO_WAY, 43).is_err());
    EXPECT_TRUE(wire::decode_reply_status(as_message(good), ONE_WAY, 42).is_err());
    auto other_ordinal = good;
    wire::store(std::span{other_ordinal}, 8, TWO_WAY.ordinal + 1);
    EXPECT_TRUE(wire::decode_reply_status(as_message(other_ordinal), TWO_WAY, 42).is_err());

    // The txid the client passes must itself be a two-way one.
    auto zero_txid = good;
    wire::store(std::span{zero_txid}, 0, u32{0});
    EXPECT_TRUE(wire::decode_reply_status(as_message(zero_txid), TWO_WAY, 0).is_err());

    // A request is never read as a reply.
    auto request = good;
    request[4] = std::to_underlying(Direction::ToServer);
    EXPECT_TRUE(wire::decode_reply_status(as_message(request), TWO_WAY, 42).is_err());

    // No status at all.
    EXPECT_TRUE(
        wire::decode_reply_status(as_message({good.begin(), good.begin() + 19}), TWO_WAY, 42)
            .is_err());

    // A status ours::Status does not name.
    auto unknown = good;
    wire::store(std::span{unknown}, 16, std::to_underlying(Status::MaxCount));
    EXPECT_TRUE(wire::decode_reply_status(as_message(unknown), TWO_WAY, 42).is_err());

    // A failure is exactly the status form: no extra byte, a zero pad, no
    // handle.
    auto longer = good;
    longer.push_back(0);
    EXPECT_TRUE(wire::decode_reply_status(as_message(longer), TWO_WAY, 42).is_err());
    auto padded = good;
    padded[20] = 1;
    EXPECT_TRUE(wire::decode_reply_status(as_message(padded), TWO_WAY, 42).is_err());
    std::array const handle{Uandle{0x13}};
    EXPECT_TRUE(wire::decode_reply_status(as_message(good, handle), TWO_WAY, 42).is_err());
}

TEST(WireReply, OkLeavesTheBodyToTheMethod) {
    auto buffer = dirty_buffer();
    auto started = wire::start_ok_reply(buffer, 32, TWO_WAY, 5);
    ASSERT_TRUE(started.is_ok());
    auto const bytes = started.unwrap();
    EXPECT_EQ(wire::load<u32>(bytes, wire::STATUS_OFFSET), 0u);

    // An Ok reply may be longer than the status form and carry handles:
    // what it may carry is the method's codec's to check.
    std::array const handle{Uandle{0x13}};
    auto const decoded =
        wire::decode_reply_status(Message{.bytes = bytes, .handles = handle}, TWO_WAY, 5);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded.unwrap(), Status::Ok);

    EXPECT_EQ(wire::start_ok_reply(buffer, 32, ONE_WAY, 0).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(wire::start_ok_reply(buffer, 32, TWO_WAY, 0).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(wire::start_ok_reply(buffer, 20, TWO_WAY, 5).unwrap_err(), Status::InvArgs);
}

TEST(WireEpitaph, RoundTrip) {
    for (auto const status: {Status::Ok, Status::BadPath, Status::NotFound}) {
        auto buffer = dirty_buffer();
        auto encoded = wire::encode_epitaph(buffer, status);
        ASSERT_TRUE(encoded.is_ok());
        auto const epitaph = encoded.unwrap();
        ASSERT_EQ(epitaph.bytes.size(), wire::STATUS_FORM_SIZE);
        EXPECT_TRUE(epitaph.handles.empty());

        auto const header = wire::decode_header(epitaph.bytes, Direction::ToClient);
        ASSERT_TRUE(header.is_ok());
        EXPECT_EQ(header->txid, 0u);
        EXPECT_EQ(header->ordinal, wire::EPITAPH_ORDINAL);

        auto const decoded = wire::decode_epitaph(epitaph);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded.unwrap(), status);
    }
}

TEST(WireEpitaph, Refusals) {
    auto buffer = dirty_buffer();
    EXPECT_EQ(wire::encode_epitaph(buffer, Status::MaxCount).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(wire::encode_epitaph(std::span{buffer}.first(23), Status::Ok).unwrap_err(),
              Status::BufferTooSmall);

    auto const good = bytes_of(wire::encode_epitaph(buffer, Status::NotFound).unwrap());
    ASSERT_TRUE(wire::decode_epitaph(as_message(good)).is_ok());

    auto with_txid = good;
    wire::store(std::span{with_txid}, 0, u32{1});
    EXPECT_TRUE(wire::decode_epitaph(as_message(with_txid)).is_err());

    auto not_epitaph = good;
    wire::store(std::span{not_epitaph}, 8, TWO_WAY.ordinal);
    EXPECT_TRUE(wire::decode_epitaph(as_message(not_epitaph)).is_err());

    auto to_server = good;
    to_server[4] = std::to_underlying(Direction::ToServer);
    EXPECT_TRUE(wire::decode_epitaph(as_message(to_server)).is_err());

    auto longer = good;
    longer.push_back(0);
    EXPECT_TRUE(wire::decode_epitaph(as_message(longer)).is_err());
    EXPECT_TRUE(wire::decode_epitaph(as_message({good.begin(), good.end() - 1})).is_err());

    auto padded = good;
    padded[23] = 1;
    EXPECT_TRUE(wire::decode_epitaph(as_message(padded)).is_err());

    auto unknown = good;
    wire::store(std::span{unknown}, 16, std::to_underlying(Status::MaxCount));
    EXPECT_TRUE(wire::decode_epitaph(as_message(unknown)).is_err());

    std::array const handle{Uandle{0x13}};
    EXPECT_TRUE(wire::decode_epitaph(as_message(good, handle)).is_err());
}

TEST(WireEpitaph, IsEpitaphLooksAtTheOrdinal) {
    // What a client checks before decoding a reply: an epitaph's header says
    // so, and no reply's does, whatever its status.
    auto buffer = dirty_buffer();
    auto const epitaph = wire::encode_epitaph(buffer, Status::AccessDenied).unwrap();
    auto const epitaph_header = wire::decode_header(epitaph.bytes, Direction::ToClient);
    ASSERT_TRUE(epitaph_header.is_ok());
    EXPECT_TRUE(wire::is_epitaph(epitaph_header.unwrap()));

    auto reply_buffer = dirty_buffer();
    auto const reply = encoded_status_reply(reply_buffer, 7, Status::AccessDenied);
    auto const reply_header = wire::decode_header(reply.bytes, Direction::ToClient);
    ASSERT_TRUE(reply_header.is_ok());
    EXPECT_FALSE(wire::is_epitaph(reply_header.unwrap()));

    // The ordinal alone decides; the rest is decode_epitaph()'s to check.
    static_assert(wire::is_epitaph(Header{.txid = 3,
                                          .direction = Direction::ToServer,
                                          .reserved = {},
                                          .magic = wire::MAGIC,
                                          .ordinal = wire::EPITAPH_ORDINAL}));
    static_assert(!wire::is_epitaph(Header{.txid = 0,
                                           .direction = Direction::ToClient,
                                           .reserved = {},
                                           .magic = wire::MAGIC,
                                           .ordinal = TWO_WAY.ordinal}));
}

TEST(WireHandle, RoundTripAndRefusals) {
    auto bytes = dirty_buffer();
    std::array<Uandle, 2> handles{};
    auto const span = std::span{bytes}.first(32);

    ASSERT_EQ(wire::encode_handle(span, 16, handles, 0, Uandle{0x23}), Status::Ok);
    ASSERT_EQ(wire::encode_handle(span, 24, handles, 1, Uandle{0x33}), Status::Ok);
    EXPECT_EQ(wire::load<u32>(span, 16), wire::HANDLE_PRESENT);
    EXPECT_EQ(wire::load<u32>(span, 24), wire::HANDLE_PRESENT);

    auto const message = Message{.bytes = span, .handles = handles};
    EXPECT_EQ(wire::decode_handle(message, 16, 0).unwrap(), Uandle{0x23});
    EXPECT_EQ(wire::decode_handle(message, 24, 1).unwrap(), Uandle{0x33});

    // The field must say a handle is there, and the handle must be.
    EXPECT_TRUE(wire::decode_handle(message, 20, 0).is_err());
    EXPECT_TRUE(
        wire::decode_handle(Message{.bytes = span, .handles = std::span{handles}.first(1)}, 24, 1)
            .is_err());
    std::array const invalid{Uandle::Invalid};
    EXPECT_TRUE(wire::decode_handle(Message{.bytes = span, .handles = invalid}, 16, 0).is_err());

    // Every handle field is required, and the handle array must have room.
    EXPECT_EQ(wire::encode_handle(span, 16, handles, 0, Uandle::Invalid), Status::InvArgs);
    EXPECT_EQ(wire::encode_handle(span, 16, handles, 2, Uandle{0x23}), Status::BufferTooSmall);
}

// SHA-256 and the ordinals computed from it (<wire/sha256.hpp>, ordinal_of()).
//
// The answers below were computed outside this file: the first three digests
// are FIPS 180-4's own examples, the rest come from Python's hashlib, and the
// ordinals of the codec headers are the numbers that were typed into them by
// hand before they were computed (see GOLDEN).

/// The digest a hex string spells, its spaces ignored: how the standard prints
/// its examples. A malformed string aborts, which in a constant expression is
/// a compile error.
constexpr auto digest_from_hex(std::string_view const hex) -> wire::Sha256Digest {
    auto const nibble = [](char const c) -> u8 {
        if (c >= '0' && c <= '9') {
            return static_cast<u8>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<u8>(c - 'a' + 10);
        }
        std::abort();
    };
    auto digest = wire::Sha256Digest{};
    auto count = usize{0};
    for (auto const c: hex) {
        if (c == ' ') {
            continue;
        }
        if (count >= 2 * digest.size()) {
            std::abort();
        }
        auto &byte = digest[count / 2];
        byte = static_cast<u8>((byte << 4) | nibble(c));
        ++count;
    }
    if (count != 2 * digest.size()) {
        std::abort();
    }
    return digest;
}

constexpr auto FIPS_EMPTY = std::string_view{};
constexpr auto FIPS_ABC = std::string_view{"abc"};
// 56 bytes: its padding does not fit the block its bytes end in, so it is
// hashed in two.
constexpr auto FIPS_TWO_BLOCK =
    std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"};
static_assert(FIPS_TWO_BLOCK.size() == 56);

constexpr auto FIPS_EMPTY_DIGEST =
    digest_from_hex("e3b0c442 98fc1c14 9afbf4c8 996fb924 27ae41e4 649b934c a495991b 7852b855");
constexpr auto FIPS_ABC_DIGEST =
    digest_from_hex("ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad");
constexpr auto FIPS_TWO_BLOCK_DIGEST =
    digest_from_hex("248d6a61 d20638b8 e5c02693 0c3e6039 a33ce459 64ff2167 f6ecedd4 19db06c1");

static_assert(wire::sha256(FIPS_EMPTY) == FIPS_EMPTY_DIGEST);
static_assert(wire::sha256(FIPS_ABC) == FIPS_ABC_DIGEST);
static_assert(wire::sha256(FIPS_TWO_BLOCK) == FIPS_TWO_BLOCK_DIGEST);

TEST(WireSha256, FipsKnownAnswers) {
    EXPECT_EQ(wire::sha256(FIPS_EMPTY), FIPS_EMPTY_DIGEST);
    EXPECT_EQ(wire::sha256(FIPS_ABC), FIPS_ABC_DIGEST);
    EXPECT_EQ(wire::sha256(FIPS_TWO_BLOCK), FIPS_TWO_BLOCK_DIGEST);
}

/// |Size| bytes that run through the alphabet, so that a byte hashed twice,
/// dropped or out of place changes the digest.
template <usize Size>
constexpr auto patterned() -> std::array<char, Size> {
    auto bytes = std::array<char, Size>{};
    for (auto i = usize{0}; i < Size; ++i) {
        bytes[i] = static_cast<char>('a' + i % 26);
    }
    return bytes;
}

/// Static storage, so that a view of it can be the message of a constant
/// expression.
template <usize Size>
inline constexpr auto PATTERNED = patterned<Size>();

template <usize Size>
constexpr auto patterned_view() -> std::string_view {
    return {PATTERNED<Size>.data(), Size};
}

struct PaddingCase {
    usize size;
    wire::Sha256Digest digest;
};

// The sizes where the padding changes shape. A block holds 64 bytes, and the
// padding is a 0x80 byte and an 8-byte length: with 55 bytes in the last block
// it fits exactly, with 56 it spills into a block of its own; 0 bytes in the
// last block (64, 128) is a block of padding alone. 255 is the longest
// selector this library promises to hash.
constexpr auto PADDING_CASES = std::array{
    PaddingCase{55,
                digest_from_hex("595615db e4f0f407 ae397d08 b4c2cb87 0cb9b0e1 1937416f 950c5160 "
                                "acf9c005")},
    PaddingCase{56,
                digest_from_hex("784f623b 78749507 8e93ff28 a25b581d f0584055 a7e71d8c d90c4547 "
                                "16b92f51")},
    PaddingCase{63,
                digest_from_hex("5ca3e1ef 5207490e ac01a795 e5cc94d5 9582a511 8bf95346 65c8668d "
                                "87aa647c")},
    PaddingCase{64,
                digest_from_hex("2fcd5a0d 60e4c941 381fcc4e 00a4bf8b e422c3dd fafb93c8 09e8d1e2 "
                                "bfffae8e")},
    PaddingCase{65,
                digest_from_hex("1b3cd187 7ab2f2f1 9f7be001 722554f3 36cb799d f0329de0 bb4c118d "
                                "c6abc06d")},
    PaddingCase{119,
                digest_from_hex("faef67da 856d6fd9 c8d12f9e d0a4fefd 3cf0ce08 5ab43e29 07418d45 "
                                "7e3c354b")},
    PaddingCase{120,
                digest_from_hex("c9512b08 619c19fb b503c7da 6b46ef20 301e5f7a 7a5f4398 91823985 "
                                "36f5c5c8")},
    PaddingCase{128,
                digest_from_hex("6c05be2c 4268843a e47e68e6 11277ce6 2c02153f 2f4d2e1e 2a1a4b44 "
                                "f766cf74")},
    PaddingCase{255,
                digest_from_hex("00adbd49 87851c62 1acc01a2 a25f3d57 f690ed26 184c0727 dd065d4c "
                                "360671f1")},
};

template <usize... Index>
constexpr auto padding_digests(std::index_sequence<Index...>)
    -> std::array<wire::Sha256Digest, sizeof...(Index)> {
    return {wire::sha256(patterned_view<PADDING_CASES[Index].size>())...};
}

constexpr auto PADDING_DIGESTS = padding_digests(std::make_index_sequence<PADDING_CASES.size()>{});

constexpr auto padding_digests_match() -> bool {
    for (auto i = usize{0}; i < PADDING_CASES.size(); ++i) {
        if (PADDING_DIGESTS[i] != PADDING_CASES[i].digest) {
            return false;
        }
    }
    return true;
}
static_assert(padding_digests_match());

TEST(WireSha256, PaddingBoundaries) {
    for (auto i = usize{0}; i < PADDING_CASES.size(); ++i) {
        EXPECT_EQ(PADDING_DIGESTS[i], PADDING_CASES[i].digest) << PADDING_CASES[i].size << " bytes";
    }
}

TEST(WireSha256, RunsAtRunTimeToo) {
    // The same function on bytes the compiler never saw.
    auto const message = std::string{PATTERNED<255>.data(), 255};
    EXPECT_EQ(wire::sha256(message), PADDING_CASES.back().digest);
    EXPECT_EQ(wire::sha256(std::string{FIPS_TWO_BLOCK}), FIPS_TWO_BLOCK_DIGEST);
}

/// A selector and the ordinal that was typed beside it in a codec header, from
/// the day those numbers were typed by hand and checked by a script. They are
/// the wire format: a peer that does not know this library sends them, so a
/// change to any one is a protocol break, whatever else is true of it.
struct Golden {
    std::string_view selector;
    u64 ordinal;
};

constexpr auto GOLDEN = std::array{
    // prot::io
    Golden{"ours.io/Directory.Open", 0x3d1d'dd82'1049'bce2},
    Golden{"ours.io/Node.Close", 0x67bf'e5a2'cd64'acda},
    Golden{"ours.io/Cloneable.Clone", 0x151b'66f7'6607'428b},
    Golden{"ours.io/File.Read", 0x2348'9a6f'12eb'b0f0},
    Golden{"ours.io/File.GetBackingMemory", 0x13b0'3c8a'57b0'6719},
    Golden{"ours.io/Directory.ReadDirents", 0x3acf'c05d'99ef'c34b},
    // prot::process
    Golden{"ours.process/Launcher.AddArgs", 0x6177'27c9'8e66'bde8},
    Golden{"ours.process/Launcher.AddHandles", 0x3292'5139'9bdc'bf52},
    Golden{"ours.process/Launcher.AddNames", 0x0987'826f'a969'0f15},
    Golden{"ours.process/Launcher.Launch", 0x4842'5238'011e'a49e},
    // prot::resource
    Golden{"ours.resource/Debug.Get", 0x226c'd5bb'1821'8fda},
    Golden{"ours.resource/Irq.Get", 0x273c'3e02'af63'50e0},
    Golden{"ours.resource/Ioport.Get", 0x231b'c3b7'4044'47b5},
    Golden{"ours.resource/Sampling.Get", 0x62a8'8580'9507'5fb4},
    Golden{"ours.kernel/RootJob.Get", 0x765b'31d7'e30c'32b5},
    // ours_examples_echo
    Golden{"ours.examples.echo/Echo.EchoString", 0x6e80'6bd7'7638'6789},
    Golden{"ours.examples.echo/Echo.Stop", 0x1156'5ac0'dbe3'72a7},
    // ours_examples_hello
    Golden{"ours.examples.hello/Hello.Greet", 0x0b34'f151'd5b5'de80},
    Golden{"ours.examples.hello/Hello.Stop", 0x3b42'cec6'd122'813b},
};
static_assert(GOLDEN.size() == 19);

template <usize... Index>
consteval auto golden_ordinals(std::index_sequence<Index...>) -> std::array<u64, sizeof...(Index)> {
    return {wire::ordinal_of(GOLDEN[Index].selector)...};
}

constexpr auto COMPUTED = golden_ordinals(std::make_index_sequence<GOLDEN.size()>{});

constexpr auto computed_match_golden() -> bool {
    for (auto i = usize{0}; i < GOLDEN.size(); ++i) {
        if (COMPUTED[i] != GOLDEN[i].ordinal) {
            return false;
        }
    }
    return true;
}
static_assert(computed_match_golden());

TEST(WireOrdinal, EveryCodecOrdinalIsItsSelectorsHash) {
    for (auto i = usize{0}; i < GOLDEN.size(); ++i) {
        EXPECT_EQ(COMPUTED[i], GOLDEN[i].ordinal) << GOLDEN[i].selector;
    }
}

TEST(WireOrdinal, GoldenOrdinalsAreDistinctAndKeepTheTopBitClear) {
    // No ordinal may equal the epitaph's, whose top bit is set.
    for (auto i = usize{0}; i < GOLDEN.size(); ++i) {
        EXPECT_EQ(GOLDEN[i].ordinal >> 63, 0u) << GOLDEN[i].selector;
        EXPECT_NE(GOLDEN[i].ordinal, wire::EPITAPH_ORDINAL) << GOLDEN[i].selector;
        for (auto j = i + 1; j < GOLDEN.size(); ++j) {
            EXPECT_NE(GOLDEN[i].ordinal, GOLDEN[j].ordinal)
                << GOLDEN[i].selector << " and " << GOLDEN[j].selector;
        }
    }
}

// The top bit of the first 8 digest bytes is cleared. "ours.io/File.Read"
// hashes to a digest whose byte 7 is 0xa3, so read little-endian the 8 bytes
// are 0xa348'9a6f'12eb'b0f0, and the ordinal is that without the top bit.
// Without the clear, this is the assertion that fails.
constexpr auto TOP_BIT_SELECTOR = std::string_view{"ours.io/File.Read"};
constexpr auto TOP_BIT_DIGEST = wire::sha256(TOP_BIT_SELECTOR);
static_assert(TOP_BIT_DIGEST[7] == 0xa3);
static_assert(wire::ordinal_of(TOP_BIT_SELECTOR) == 0x2348'9a6f'12eb'b0f0);
// And a selector whose digest has the top bit clear is left as it is.
static_assert(wire::sha256("ours.io/Directory.Open")[7] == 0x3d);
static_assert(wire::ordinal_of("ours.io/Directory.Open") == 0x3d1d'dd82'1049'bce2);

// The order of the 8 bytes. The digest of "abc" starts ba 78 16 bf 8f 01 cf ea:
// read little-endian that is 0xeacf'018f'bf16'78ba, read big-endian it would
// be 0xba78'16bf'8f01'cfea.
static_assert(wire::ordinal_of("abc") == 0x6acf'018f'bf16'78ba);

// Padding boundaries, through the ordinal: a selector of each length hashes
// like the message of that length.
static_assert(wire::ordinal_of(patterned_view<55>()) == 0x07f4'f0e4'db15'5659);
static_assert(wire::ordinal_of(patterned_view<56>()) == 0x0795'7478'3b62'4f78);
static_assert(wire::ordinal_of(patterned_view<64>()) == 0x41c9'e460'0d5a'cd2f);
static_assert(wire::ordinal_of(patterned_view<255>()) == 0x621c'8587'49bd'ad00);

TEST(WireOrdinal, TopBitIsCleared) {
    // The raw value really has the bit set, or this tests nothing.
    EXPECT_EQ(TOP_BIT_DIGEST[7] >> 7, 1);
    EXPECT_EQ(wire::ordinal_of(TOP_BIT_SELECTOR) >> 63, 0u);
    EXPECT_EQ(wire::ordinal_of(TOP_BIT_SELECTOR), 0x2348'9a6f'12eb'b0f0u);
}

TEST(WireOrdinal, ReadsTheDigestLittleEndian) {
    EXPECT_EQ(wire::ordinal_of("abc"), 0x6acf'018f'bf16'78bau);
}

TEST(WireOrdinal, SelectorsAtThePaddingBoundaries) {
    EXPECT_EQ(wire::ordinal_of(patterned_view<55>()), 0x07f4'f0e4'db15'5659u);
    EXPECT_EQ(wire::ordinal_of(patterned_view<56>()), 0x0795'7478'3b62'4f78u);
    EXPECT_EQ(wire::ordinal_of(patterned_view<64>()), 0x41c9'e460'0d5a'cd2fu);
    EXPECT_EQ(wire::ordinal_of(patterned_view<255>()), 0x621c'8587'49bd'ad00u);
}

constexpr auto SELECTED = wire::method("ours.io/File.Read", MethodKind::TwoWay);
static_assert(SELECTED.selector == "ours.io/File.Read");
static_assert(SELECTED.ordinal == 0x2348'9a6f'12eb'b0f0);
static_assert(SELECTED.kind == MethodKind::TwoWay);

TEST(WireMethod, MethodIsASelectorItsOrdinalAndItsKind) {
    EXPECT_EQ(SELECTED.selector, "ours.io/File.Read");
    EXPECT_EQ(SELECTED.ordinal, wire::ordinal_of("ours.io/File.Read"));
    EXPECT_EQ(SELECTED.kind, MethodKind::TwoWay);
    EXPECT_EQ(wire::method("ours.io/Cloneable.Clone", MethodKind::OneWay).kind, MethodKind::OneWay);
}

constexpr auto FIRST = wire::method("test/Protocol.First", MethodKind::TwoWay);
constexpr auto SECOND = wire::method("test/Protocol.Second", MethodKind::OneWay);
constexpr auto THIRD = wire::method("test/Protocol.Third", MethodKind::TwoWay);
// A collision cannot be found by hashing, so it is written out: a method of
// another name with FIRST's ordinal.
constexpr auto CLASHING = Method{
    .selector = "test/Protocol.Clashing",
    .ordinal = FIRST.ordinal,
    .kind = MethodKind::OneWay,
};

static_assert(wire::all_distinct());
static_assert(wire::all_distinct(FIRST));
static_assert(wire::all_distinct(FIRST, SECOND, THIRD));
// The same method declared twice.
static_assert(!wire::all_distinct(FIRST, SECOND, FIRST));
// Two names, one ordinal; the pair need not be neighbours.
static_assert(!wire::all_distinct(FIRST, CLASHING));
static_assert(!wire::all_distinct(FIRST, SECOND, THIRD, CLASHING));

TEST(WireMethod, AllDistinct) {
    EXPECT_TRUE(wire::all_distinct(FIRST, SECOND, THIRD));
    EXPECT_FALSE(wire::all_distinct(FIRST, SECOND, FIRST));
    EXPECT_FALSE(wire::all_distinct(SECOND, THIRD, CLASHING, FIRST));
}
} // namespace
