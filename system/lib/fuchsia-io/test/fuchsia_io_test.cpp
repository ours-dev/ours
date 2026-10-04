/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The fuchsia.io codec (<fuchsia_io/fuchsia_io.hpp>): every request and reply
// is encoded, decoded back, and then broken on purpose, one malformed form at
// a time. The root decodes what programs it does not trust send it, and a
// client decodes what a server it does not trust answers.
#include <fuchsia_io/fuchsia_io.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <wire/wire.hpp>

namespace {
using fuchsia_io::Dirent;
using fuchsia_io::DirentReader;
using fuchsia_io::DirentType;
using fuchsia_io::DirentWriter;
using fuchsia_io::Flags;
using fuchsia_io::VmoFlags;
using ours::Status;
using ours::Uandle;
using wire::Message;

constexpr auto FILL = u8{0xCC};
constexpr auto OBJECT = Uandle{0x1003};
constexpr auto VMO = Uandle{0x2003};

/// Room for one message, starting dirty so that a byte the encoder forgot
/// shows.
struct Buffer {
    std::vector<u8> bytes = std::vector<u8>(wire::MAX_MESSAGE_BYTES, FILL);
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> handles{};
};

/// A message copied out of the buffer it was encoded in, so that a test can
/// break it.
struct Owned {
    std::vector<u8> bytes;
    std::vector<Uandle> handles;

    explicit Owned(Message const message)
        : bytes(message.bytes.begin(), message.bytes.end())
        , handles(message.handles.begin(), message.handles.end()) {}

    [[nodiscard]]
    auto view() const -> Message {
        return Message{.bytes = bytes, .handles = handles};
    }

    auto set_u32(usize const offset, u32 const value) -> void {
        wire::store(std::span{bytes}, offset, value);
    }
};

auto owned(ustl::Result<Message, Status> const &encoded) -> Owned {
    EXPECT_TRUE(encoded.is_ok());
    return Owned(encoded.unwrap());
}

// The two layouts the subset is sized against: the largest Open and the
// largest Read reply fill no more than a channel message.
static_assert(fuchsia_io::MAX_TRANSFER_SIZE == 4072);
static_assert(fuchsia_io::MAX_BUF == fuchsia_io::MAX_TRANSFER_SIZE);

// --- Directory.Open ---------------------------------------------------------

auto open_request(std::string_view const path) -> fuchsia_io::OpenRequest {
    return {
        .flags = fuchsia_io::PERM_READABLE | Flags::ProtocolFile,
        .path = path,
        .object = OBJECT,
    };
}

TEST(FuchsiaIoOpen, RoundTrip) {
    Buffer buffer;
    auto const message = owned(
        fuchsia_io::encode_open(buffer.bytes, buffer.handles, open_request("bin/routing_checks")));
    EXPECT_EQ(message.bytes.size(), 32 + std::string_view{"bin/routing_checks"}.size());
    ASSERT_EQ(message.handles.size(), 1u);
    EXPECT_EQ(message.handles[0], OBJECT);
    EXPECT_EQ(wire::load<u32>(message.bytes, 24), wire::HANDLE_PRESENT);

    auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->txid, 0u);
    EXPECT_EQ(header->ordinal, fuchsia_io::OPEN.ordinal);

    auto const decoded = fuchsia_io::decode_open(message.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->flags, fuchsia_io::PERM_READABLE | Flags::ProtocolFile);
    EXPECT_EQ(decoded->path, "bin/routing_checks");
    EXPECT_EQ(decoded->object, OBJECT);
}

TEST(FuchsiaIoOpen, WhatTheServerJudgesStillDecodes) {
    // An empty path is BadPath and a bit nobody defines is the server's to
    // judge (Flags is flexible): both are well formed.
    auto const flags = static_cast<Flags>(u64{1} << 62) | Flags::PermExecute;
    for (auto const path: {std::string_view{}, std::string_view{"."}, std::string_view{"/x/"}}) {
        Buffer buffer;
        auto request = open_request(path);
        request.flags = flags;
        auto const message = owned(fuchsia_io::encode_open(buffer.bytes, buffer.handles, request));
        auto const decoded = fuchsia_io::decode_open(message.view());
        ASSERT_TRUE(decoded.is_ok()) << "path \"" << path << "\"";
        EXPECT_EQ(decoded->path, path);
        EXPECT_EQ(decoded->flags, flags);
    }
}

TEST(FuchsiaIoOpen, LongestPath) {
    Buffer buffer;
    std::string const longest(fuchsia_io::MAX_PATH_LENGTH, 'a');
    auto const message =
        owned(fuchsia_io::encode_open(buffer.bytes, buffer.handles, open_request(longest)));
    EXPECT_EQ(message.bytes.size(), 32 + fuchsia_io::MAX_PATH_LENGTH);
    EXPECT_EQ(fuchsia_io::decode_open(message.view())->path, longest);

    std::string const too_long(fuchsia_io::MAX_PATH_LENGTH + 1, 'a');
    EXPECT_EQ(
        fuchsia_io::encode_open(buffer.bytes, buffer.handles, open_request(too_long)).unwrap_err(),
        Status::InvArgs);
}

TEST(FuchsiaIoOpen, EncodeRefusals) {
    Buffer buffer;
    auto request = open_request("x");
    request.object = Uandle::Invalid;
    EXPECT_EQ(fuchsia_io::encode_open(buffer.bytes, buffer.handles, request).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(fuchsia_io::encode_open(buffer.bytes, {}, open_request("x")).unwrap_err(),
              Status::BufferTooSmall);
    EXPECT_EQ(fuchsia_io::encode_open(std::span{buffer.bytes}.first(32), buffer.handles,
                                      open_request("x"))
                  .unwrap_err(),
              Status::BufferTooSmall);
}

TEST(FuchsiaIoOpen, DecodeRefusals) {
    Buffer buffer;
    auto const good =
        owned(fuchsia_io::encode_open(buffer.bytes, buffer.handles, open_request("bin/x")));
    ASSERT_TRUE(fuchsia_io::decode_open(good.view()).is_ok());

    auto refused = [](Owned const &message) {
        auto const decoded = fuchsia_io::decode_open(message.view());
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };

    // A path that runs past the message, stops short of its end, or is
    // longer than any Open may carry.
    for (auto const path_size:
         {u32{6}, u32{4}, u32{fuchsia_io::MAX_PATH_LENGTH + 1}, u32{0xFFFF'FFFF}}) {
        auto broken = good;
        broken.set_u32(28, path_size);
        EXPECT_TRUE(refused(broken)) << "path_size " << path_size;
    }
    // Cut inside the fixed part.
    auto cut = good;
    cut.bytes.resize(31);
    EXPECT_TRUE(refused(cut));

    // The server end is required, and it is the only handle.
    auto no_handle = good;
    no_handle.handles.clear();
    EXPECT_TRUE(refused(no_handle));
    auto two_handles = good;
    two_handles.handles.push_back(Uandle{0x3003});
    EXPECT_TRUE(refused(two_handles));
    auto invalid = good;
    invalid.handles[0] = Uandle::Invalid;
    EXPECT_TRUE(refused(invalid));
    auto absent = good;
    absent.set_u32(24, 0);
    EXPECT_TRUE(refused(absent));

    // Open is one-way: no txid.
    auto with_txid = good;
    with_txid.set_u32(0, 1);
    EXPECT_TRUE(refused(with_txid));

    // Another method's request, and a reply's direction.
    Buffer other;
    auto const clone = owned(fuchsia_io::encode_clone(other.bytes, other.handles, {OBJECT}));
    EXPECT_TRUE(refused(clone));
    auto reply = good;
    reply.bytes[4] = std::to_underlying(wire::Direction::ToClient);
    EXPECT_TRUE(refused(reply));
}

// --- Node.Close -------------------------------------------------------------

TEST(FuchsiaIoClose, RoundTrip) {
    Buffer buffer;
    auto const request = owned(fuchsia_io::encode_close(buffer.bytes, {.txid = 3}));
    EXPECT_EQ(request.bytes.size(), wire::HEADER_SIZE);
    EXPECT_TRUE(request.handles.empty());
    EXPECT_EQ(fuchsia_io::decode_close(request.view())->txid, 3u);

    for (auto const status: {Status::Ok, Status::BadState}) {
        Buffer reply_buffer;
        auto const reply =
            owned(fuchsia_io::encode_close_reply(reply_buffer.bytes, 3, {.status = status}));
        EXPECT_EQ(reply.bytes.size(), wire::STATUS_FORM_SIZE);
        auto const decoded = fuchsia_io::decode_close_reply(reply.view(), 3);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded->status, status);
    }
}

TEST(FuchsiaIoClose, Refusals) {
    Buffer buffer;
    EXPECT_EQ(fuchsia_io::encode_close(buffer.bytes, {.txid = 0}).unwrap_err(), Status::InvArgs);

    auto const good = owned(fuchsia_io::encode_close(buffer.bytes, {.txid = 3}));
    auto longer = good;
    longer.bytes.resize(24, 0);
    EXPECT_TRUE(fuchsia_io::decode_close(longer.view()).is_err());
    auto handle = good;
    handle.handles.push_back(OBJECT);
    EXPECT_TRUE(fuchsia_io::decode_close(handle.view()).is_err());

    // Close returns nothing: even its Ok reply is the status form.
    Buffer reply_buffer;
    auto const reply =
        owned(fuchsia_io::encode_close_reply(reply_buffer.bytes, 3, {.status = Status::Ok}));
    EXPECT_TRUE(fuchsia_io::decode_close_reply(reply.view(), 4).is_err());
    auto padded = reply;
    padded.set_u32(20, 1);
    EXPECT_TRUE(fuchsia_io::decode_close_reply(padded.view(), 3).is_err());
    auto reply_longer = reply;
    reply_longer.bytes.push_back(0);
    EXPECT_TRUE(fuchsia_io::decode_close_reply(reply_longer.view(), 3).is_err());
    auto reply_handle = reply;
    reply_handle.handles.push_back(OBJECT);
    EXPECT_TRUE(fuchsia_io::decode_close_reply(reply_handle.view(), 3).is_err());
}

// --- Cloneable.Clone --------------------------------------------------------

TEST(FuchsiaIoClone, RoundTrip) {
    Buffer buffer;
    auto const message =
        owned(fuchsia_io::encode_clone(buffer.bytes, buffer.handles, {.request = OBJECT}));
    EXPECT_EQ(message.bytes.size(), 24u);
    ASSERT_EQ(message.handles.size(), 1u);
    EXPECT_EQ(fuchsia_io::decode_clone(message.view())->request, OBJECT);
}

TEST(FuchsiaIoClone, Refusals) {
    Buffer buffer;
    EXPECT_EQ(fuchsia_io::encode_clone(buffer.bytes, buffer.handles, {.request = Uandle::Invalid})
                  .unwrap_err(),
              Status::InvArgs);

    auto const good =
        owned(fuchsia_io::encode_clone(buffer.bytes, buffer.handles, {.request = OBJECT}));
    auto refused = [](Owned const &message) {
        return fuchsia_io::decode_clone(message.view()).is_err();
    };
    auto padded = good;
    padded.set_u32(20, 1);
    EXPECT_TRUE(refused(padded));
    auto no_handle = good;
    no_handle.handles.clear();
    EXPECT_TRUE(refused(no_handle));
    auto two = good;
    two.handles.push_back(VMO);
    EXPECT_TRUE(refused(two));
    auto absent = good;
    absent.set_u32(16, 0);
    EXPECT_TRUE(refused(absent));
    auto shorter = good;
    shorter.bytes.pop_back();
    EXPECT_TRUE(refused(shorter));
    auto with_txid = good;
    with_txid.set_u32(0, 2);
    EXPECT_TRUE(refused(with_txid));
}

// --- File.Read --------------------------------------------------------------

TEST(FuchsiaIoRead, RoundTrip) {
    Buffer buffer;
    auto const request =
        owned(fuchsia_io::encode_read(buffer.bytes, {.txid = 7, .count = u64{1} << 40}));
    EXPECT_EQ(request.bytes.size(), 24u);
    auto const decoded = fuchsia_io::decode_read(request.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, 7u);
    // Above MAX_TRANSFER_SIZE, which the server answers with OutOfRange: the
    // codec carries it.
    EXPECT_EQ(decoded->count, u64{1} << 40);

    std::vector<u8> const data{'\x7f', 'E', 'L', 'F', 0, 1};
    Buffer reply_buffer;
    auto const reply = owned(
        fuchsia_io::encode_read_reply(reply_buffer.bytes, 7, {.status = Status::Ok, .data = data}));
    EXPECT_EQ(reply.bytes.size(), 24 + data.size());
    auto const read = fuchsia_io::decode_read_reply(reply.view(), 7);
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(read->status, Status::Ok);
    EXPECT_TRUE(std::ranges::equal(read->data, data));

    // End of file is an Ok reply with no data.
    Buffer eof_buffer;
    auto const eof =
        owned(fuchsia_io::encode_read_reply(eof_buffer.bytes, 7, {.status = Status::Ok}));
    EXPECT_EQ(eof.bytes.size(), 24u);
    EXPECT_TRUE(fuchsia_io::decode_read_reply(eof.view(), 7)->data.empty());

    Buffer error_buffer;
    auto const error = owned(
        fuchsia_io::encode_read_reply(error_buffer.bytes, 7, {.status = Status::AccessDenied}));
    auto const denied = fuchsia_io::decode_read_reply(error.view(), 7);
    ASSERT_TRUE(denied.is_ok());
    EXPECT_EQ(denied->status, Status::AccessDenied);
    EXPECT_TRUE(denied->data.empty());
}

TEST(FuchsiaIoRead, LargestReplyFillsOneMessage) {
    Buffer buffer;
    std::vector<u8> const data(fuchsia_io::MAX_TRANSFER_SIZE, 0x5A);
    auto const reply =
        owned(fuchsia_io::encode_read_reply(buffer.bytes, 1, {.status = Status::Ok, .data = data}));
    EXPECT_EQ(reply.bytes.size(), wire::MAX_MESSAGE_BYTES);
    EXPECT_EQ(fuchsia_io::decode_read_reply(reply.view(), 1)->data.size(),
              fuchsia_io::MAX_TRANSFER_SIZE);

    std::vector<u8> const too_much(fuchsia_io::MAX_TRANSFER_SIZE + 1, 0x5A);
    EXPECT_EQ(
        fuchsia_io::encode_read_reply(buffer.bytes, 1, {.status = Status::Ok, .data = too_much})
            .unwrap_err(),
        Status::InvArgs);
}

TEST(FuchsiaIoRead, DataBuiltInPlace) {
    Buffer buffer;
    auto const payload = fuchsia_io::reply_payload(buffer.bytes);
    ASSERT_EQ(payload.size(), fuchsia_io::MAX_TRANSFER_SIZE);
    EXPECT_EQ(payload.data(), buffer.bytes.data() + 24);
    std::string_view const text = "written where the reply keeps it";
    std::ranges::copy(text, payload.begin());

    auto const reply = owned(fuchsia_io::encode_read_reply(
        buffer.bytes, 2, {.status = Status::Ok, .data = payload.first(text.size())}));
    auto const read = fuchsia_io::decode_read_reply(reply.view(), 2);
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(
        (std::string_view{reinterpret_cast<char const *>(read->data.data()), read->data.size()}),
        text);

    std::array<u8, 24> small{};
    EXPECT_TRUE(fuchsia_io::reply_payload(small).empty());
}

TEST(FuchsiaIoRead, Refusals) {
    Buffer buffer;
    EXPECT_EQ(fuchsia_io::encode_read(buffer.bytes, {.txid = 0, .count = 1}).unwrap_err(),
              Status::InvArgs);
    std::array<u8, 1> const byte{1};
    EXPECT_EQ(fuchsia_io::encode_read_reply(buffer.bytes, 1,
                                            {.status = Status::AccessDenied, .data = byte})
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(fuchsia_io::encode_read_reply(std::span{buffer.bytes}.first(24), 1,
                                            {.status = Status::Ok, .data = byte})
                  .unwrap_err(),
              Status::BufferTooSmall);

    auto const request = owned(fuchsia_io::encode_read(buffer.bytes, {.txid = 7, .count = 9}));
    auto longer = request;
    longer.bytes.push_back(0);
    EXPECT_TRUE(fuchsia_io::decode_read(longer.view()).is_err());
    auto handle = request;
    handle.handles.push_back(VMO);
    EXPECT_TRUE(fuchsia_io::decode_read(handle.view()).is_err());

    std::array<u8, 3> const data{1, 2, 3};
    Buffer reply_buffer;
    auto const reply = owned(
        fuchsia_io::encode_read_reply(reply_buffer.bytes, 7, {.status = Status::Ok, .data = data}));
    auto refused = [](Owned const &message) {
        return fuchsia_io::decode_read_reply(message.view(), 7).is_err();
    };
    // A size that disagrees with the bytes, or is above the transfer limit.
    for (auto const size: {u32{2}, u32{4}, u32{fuchsia_io::MAX_TRANSFER_SIZE + 1}}) {
        auto broken = reply;
        broken.set_u32(20, size);
        EXPECT_TRUE(refused(broken)) << "size " << size;
    }
    auto reply_handle = reply;
    reply_handle.handles.push_back(VMO);
    EXPECT_TRUE(refused(reply_handle));
    // A request is not a reply, nor the reply to another request.
    EXPECT_TRUE(refused(request));
    EXPECT_TRUE(fuchsia_io::decode_read_reply(reply.view(), 8).is_err());
    // A failure that carries data anyway.
    auto failed_with_data = reply;
    failed_with_data.set_u32(16, std::to_underlying(Status::AccessDenied));
    EXPECT_TRUE(refused(failed_with_data));
}

// --- File.GetBackingMemory --------------------------------------------------

TEST(FuchsiaIoGetBackingMemory, RoundTrip) {
    // fdio_get_vmo_exec's flags.
    auto const flags = VmoFlags::Read | VmoFlags::Execute | VmoFlags::PrivateClone;
    Buffer buffer;
    auto const request =
        owned(fuchsia_io::encode_get_backing_memory(buffer.bytes, {.txid = 5, .flags = flags}));
    EXPECT_EQ(request.bytes.size(), 24u);
    auto const decoded = fuchsia_io::decode_get_backing_memory(request.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, 5u);
    EXPECT_EQ(decoded->flags, flags);

    Buffer reply_buffer;
    auto const reply = owned(fuchsia_io::encode_get_backing_memory_reply(
        reply_buffer.bytes, reply_buffer.handles, 5, {.status = Status::Ok, .vmo = VMO}));
    EXPECT_EQ(reply.bytes.size(), 24u);
    ASSERT_EQ(reply.handles.size(), 1u);
    auto const memory = fuchsia_io::decode_get_backing_memory_reply(reply.view(), 5);
    ASSERT_TRUE(memory.is_ok());
    EXPECT_EQ(memory->status, Status::Ok);
    EXPECT_EQ(memory->vmo, VMO);

    Buffer error_buffer;
    auto const error = owned(fuchsia_io::encode_get_backing_memory_reply(
        error_buffer.bytes, error_buffer.handles, 5, {.status = Status::AccessDenied}));
    EXPECT_TRUE(error.handles.empty());
    auto const denied = fuchsia_io::decode_get_backing_memory_reply(error.view(), 5);
    ASSERT_TRUE(denied.is_ok());
    EXPECT_EQ(denied->status, Status::AccessDenied);
    EXPECT_EQ(denied->vmo, Uandle::Invalid);
}

TEST(FuchsiaIoGetBackingMemory, Refusals) {
    Buffer buffer;
    // VmoFlags is strict: a bit fuchsia.io does not define is malformed.
    auto const unknown = static_cast<VmoFlags>(0x8);
    EXPECT_EQ(fuchsia_io::encode_get_backing_memory(buffer.bytes, {.txid = 1, .flags = unknown})
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(fuchsia_io::encode_get_backing_memory_reply(
                  buffer.bytes, buffer.handles, 1, {.status = Status::NotSupported, .vmo = VMO})
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(fuchsia_io::encode_get_backing_memory_reply(buffer.bytes, {}, 1,
                                                          {.status = Status::Ok, .vmo = VMO})
                  .unwrap_err(),
              Status::BufferTooSmall);

    auto const request = owned(
        fuchsia_io::encode_get_backing_memory(buffer.bytes, {.txid = 1, .flags = VmoFlags::Read}));
    auto refused = [](Owned const &message) {
        return fuchsia_io::decode_get_backing_memory(message.view()).is_err();
    };
    for (auto const bits: {u32{0x8}, u32{0x4'0000}, u32{0x8000'0000}}) {
        auto broken = request;
        broken.set_u32(16, bits | 0x1);
        EXPECT_TRUE(refused(broken)) << "flags " << bits;
    }
    auto padded = request;
    padded.set_u32(20, 1);
    EXPECT_TRUE(refused(padded));
    auto handle = request;
    handle.handles.push_back(VMO);
    EXPECT_TRUE(refused(handle));

    Buffer reply_buffer;
    auto const reply = owned(fuchsia_io::encode_get_backing_memory_reply(
        reply_buffer.bytes, reply_buffer.handles, 1, {.status = Status::Ok, .vmo = VMO}));
    auto reply_refused = [](Owned const &message) {
        return fuchsia_io::decode_get_backing_memory_reply(message.view(), 1).is_err();
    };
    auto no_vmo = reply;
    no_vmo.handles.clear();
    EXPECT_TRUE(reply_refused(no_vmo));
    auto absent = reply;
    absent.set_u32(20, 0);
    EXPECT_TRUE(reply_refused(absent));
    auto two = reply;
    two.handles.push_back(OBJECT);
    EXPECT_TRUE(reply_refused(two));
    auto longer = reply;
    longer.bytes.push_back(0);
    EXPECT_TRUE(reply_refused(longer));
    auto invalid = reply;
    invalid.handles[0] = Uandle::Invalid;
    EXPECT_TRUE(reply_refused(invalid));
    // A failure is the status form, with no handle beside it.
    auto failed_with_vmo = reply;
    failed_with_vmo.set_u32(16, std::to_underlying(Status::AccessDenied));
    EXPECT_TRUE(reply_refused(failed_with_vmo));
}

// --- Directory.ReadDirents --------------------------------------------------

TEST(FuchsiaIoReadDirents, RoundTrip) {
    Buffer buffer;
    auto const request =
        owned(fuchsia_io::encode_read_dirents(buffer.bytes, {.txid = 11, .max_bytes = 4096}));
    auto const decoded = fuchsia_io::decode_read_dirents(request.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, 11u);
    EXPECT_EQ(decoded->max_bytes, 4096u);

    // The server packs entries straight into the reply.
    Buffer reply_buffer;
    DirentWriter writer(fuchsia_io::reply_payload(reply_buffer.bytes));
    ASSERT_EQ(writer.append({.ino = 1, .type = DirentType::Directory, .name = "."}), Status::Ok);
    ASSERT_EQ(
        writer.append(
            {.ino = fuchsia_io::INO_UNKNOWN, .type = DirentType::File, .name = "routing_checks"}),
        Status::Ok);
    auto const reply = owned(fuchsia_io::encode_read_dirents_reply(
        reply_buffer.bytes, 11, {.status = Status::Ok, .dirents = writer.written()}));
    EXPECT_EQ(reply.bytes.size(), usize{24 + 10 + 1 + 10 + 14});

    auto const dirents = fuchsia_io::decode_read_dirents_reply(reply.view(), 11);
    ASSERT_TRUE(dirents.is_ok());
    EXPECT_EQ(dirents->status, Status::Ok);

    DirentReader reader(dirents->dirents);
    auto first = reader.next();
    ASSERT_TRUE(first.is_ok() && first->is_some());
    EXPECT_EQ(first->value().ino, 1u);
    EXPECT_EQ(first->value().type, DirentType::Directory);
    EXPECT_EQ(first->value().name, ".");
    auto second = reader.next();
    ASSERT_TRUE(second.is_ok() && second->is_some());
    EXPECT_EQ(second->value().ino, fuchsia_io::INO_UNKNOWN);
    EXPECT_EQ(second->value().type, DirentType::File);
    EXPECT_EQ(second->value().name, "routing_checks");
    auto end = reader.next();
    ASSERT_TRUE(end.is_ok());
    EXPECT_TRUE(end->is_none());

    // The end of the directory is an Ok reply with no dirents.
    Buffer end_buffer;
    auto const done =
        owned(fuchsia_io::encode_read_dirents_reply(end_buffer.bytes, 11, {.status = Status::Ok}));
    EXPECT_TRUE(fuchsia_io::decode_read_dirents_reply(done.view(), 11)->dirents.empty());

    Buffer small_buffer;
    auto const small = owned(fuchsia_io::encode_read_dirents_reply(
        small_buffer.bytes, 11, {.status = Status::BufferTooSmall}));
    EXPECT_EQ(fuchsia_io::decode_read_dirents_reply(small.view(), 11)->status,
              Status::BufferTooSmall);
}

TEST(FuchsiaIoReadDirents, Refusals) {
    Buffer buffer;
    auto const request =
        owned(fuchsia_io::encode_read_dirents(buffer.bytes, {.txid = 11, .max_bytes = 64}));
    auto shorter = request;
    shorter.bytes.pop_back();
    EXPECT_TRUE(fuchsia_io::decode_read_dirents(shorter.view()).is_err());
    // A Read is not a ReadDirents, though the two bodies look alike.
    Buffer read_buffer;
    auto const read = owned(fuchsia_io::encode_read(read_buffer.bytes, {.txid = 11, .count = 64}));
    EXPECT_TRUE(fuchsia_io::decode_read_dirents(read.view()).is_err());
    EXPECT_TRUE(fuchsia_io::decode_read(request.view()).is_err());

    std::vector<u8> const too_much(fuchsia_io::MAX_BUF + 1, 0);
    EXPECT_EQ(fuchsia_io::encode_read_dirents_reply(buffer.bytes, 11,
                                                    {.status = Status::Ok, .dirents = too_much})
                  .unwrap_err(),
              Status::InvArgs);

    std::array<u8, 11> const one{};
    Buffer reply_buffer;
    auto const reply = owned(fuchsia_io::encode_read_dirents_reply(
        reply_buffer.bytes, 11, {.status = Status::Ok, .dirents = one}));
    auto broken = reply;
    broken.set_u32(20, 12);
    EXPECT_TRUE(fuchsia_io::decode_read_dirents_reply(broken.view(), 11).is_err());
}

// --- Dirents ----------------------------------------------------------------

TEST(FuchsiaIoDirents, WriterStopsAtTheFirstEntryThatDoesNotFit) {
    std::array<u8, 24> buffer{};
    std::ranges::fill(buffer, FILL);
    DirentWriter writer(buffer);
    ASSERT_EQ(writer.append({.ino = 1, .type = DirentType::File, .name = "abc"}), Status::Ok);
    EXPECT_EQ(writer.written().size(), 13u);

    // Twelve bytes are needed, eleven are left: nothing is written.
    EXPECT_EQ(writer.append({.ino = 2, .type = DirentType::File, .name = "de"}),
              Status::BufferTooSmall);
    EXPECT_EQ(writer.written().size(), 13u);
    EXPECT_EQ(buffer[13], FILL);

    // One that does fit still goes in.
    ASSERT_EQ(writer.append({.ino = 3, .type = DirentType::File, .name = "f"}), Status::Ok);
    EXPECT_EQ(writer.written().size(), 24u);
}

TEST(FuchsiaIoDirents, Names) {
    std::vector<u8> buffer(1024);
    DirentWriter writer(buffer);
    EXPECT_EQ(writer.append({.ino = 1, .type = DirentType::File, .name = ""}), Status::InvArgs);
    std::string const too_long(fuchsia_io::MAX_NAME_LENGTH + 1, 'n');
    EXPECT_EQ(writer.append({.ino = 1, .type = DirentType::File, .name = too_long}),
              Status::InvArgs);
    std::string const longest(fuchsia_io::MAX_NAME_LENGTH, 'n');
    ASSERT_EQ(writer.append({.ino = 1, .type = DirentType::File, .name = longest}), Status::Ok);

    DirentReader reader(writer.written());
    auto const entry = reader.next();
    ASSERT_TRUE(entry.is_ok() && entry->is_some());
    EXPECT_EQ(entry->value().name, longest);
}

TEST(FuchsiaIoDirents, ReaderRefusals) {
    std::vector<u8> buffer(64);
    DirentWriter writer(buffer);
    ASSERT_EQ(writer.append({.ino = 9, .type = DirentType::Service, .name = "svc"}), Status::Ok);
    auto const good = std::vector<u8>(writer.written().begin(), writer.written().end());

    // Cut in the fixed part, and in the name.
    for (auto const size: {usize{1}, usize{9}, usize{12}}) {
        DirentReader reader(std::span{good}.first(size));
        EXPECT_TRUE(reader.next().is_err()) << "size " << size;
    }

    // An empty name; and the reader stays on the bad entry.
    auto empty_name = good;
    empty_name[8] = 0;
    DirentReader reader(empty_name);
    EXPECT_TRUE(reader.next().is_err());
    EXPECT_TRUE(reader.next().is_err());

    // A type fuchsia.io does not define reads back: DirentType is flexible.
    auto unknown_type = good;
    unknown_type[9] = 99;
    DirentReader flexible(unknown_type);
    auto const entry = flexible.next();
    ASSERT_TRUE(entry.is_ok() && entry->is_some());
    EXPECT_EQ(std::to_underlying(entry->value().type), 99);
}

// --- The epitaph ------------------------------------------------------------

TEST(FuchsiaIoEpitaph, AFailedOpenIsAnEpitaphOnItsObject) {
    // The server answers Open on the new channel, with the status form and
    // the epitaph ordinal; nothing on the directory's own channel.
    Buffer buffer;
    auto const epitaph = owned(wire::encode_epitaph(buffer.bytes, Status::BadPath));
    auto const header = wire::decode_header(epitaph.bytes, wire::Direction::ToClient);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->ordinal, wire::EPITAPH_ORDINAL);
    EXPECT_EQ(wire::decode_epitaph(epitaph.view()).unwrap(), Status::BadPath);

    // A client that expected a Read reply refuses it as one, and so learns to
    // read it as an epitaph instead.
    EXPECT_TRUE(fuchsia_io::decode_read_reply(epitaph.view(), 0).is_err());
}
} // namespace
