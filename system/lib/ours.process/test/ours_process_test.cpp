/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The ours.process.Launcher codec (<ours_process/ours_process.hpp>):
// every request and the reply are encoded, decoded back, and then broken on
// purpose, one malformed form at a time. The root decodes what programs it
// does not trust send its Launcher, and a client decodes what the root
// answers.
#include <ours_process/ours_process.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <ours/process_args.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;
using ours_process::HandleInfo;
using ours_process::LaunchReply;
using ours_process::LaunchRequest;
using ours_process::NameInfo;
using ours_process::PackedStrings;
using ours_process::StringReader;
using wire::Message;

constexpr auto FILL = u8{0xCC};
constexpr auto JOB = Uandle{0x1003};
constexpr auto EXECUTABLE = Uandle{0x2003};
constexpr auto PROCESS = Uandle{0x3003};

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

    /// The message cut to its first |size| bytes, with the rest still in
    /// memory right behind the cut, so that a decoder reading past its
    /// message finds valid bytes there and accepts, and the test sees it.
    [[nodiscard]]
    auto cut(usize const size) const -> Message {
        return Message{.bytes = std::span{bytes}.first(size), .handles = handles};
    }

    auto set_u32(usize const offset, u32 const value) -> void {
        wire::store(std::span{bytes}, offset, value);
    }
};

auto owned(ustl::Result<Message, Status> const &encoded) -> Owned {
    EXPECT_TRUE(encoded.is_ok());
    return Owned(encoded.unwrap());
}

/// Every string of |strings|, read back.
auto read_all(PackedStrings const strings) -> std::vector<std::string> {
    std::vector<std::string> out;
    auto reader = StringReader(strings);
    for (auto string = reader.next(); string.is_some(); string = reader.next()) {
        out.emplace_back(*string);
    }
    return out;
}

auto handle(usize const index) -> Uandle {
    return static_cast<Uandle>(0x4003 + (index << 8));
}

// --- Launcher.AddArgs -------------------------------------------------------

TEST(OursProcessAddArgs, RoundTrip) {
    Buffer buffer;
    auto const args = std::array<std::string_view, 3>{"routing_checks", "", "child"};
    auto const message = owned(ours_process::encode_add_args(buffer.bytes, args));
    // 24, then "routing_checks\0" (15), "\0" (1) and "child\0" (6).
    EXPECT_EQ(message.bytes.size(), 24u + 15 + 1 + 6);
    EXPECT_TRUE(message.handles.empty());
    EXPECT_EQ(wire::load<u32>(message.bytes, 16), 3u);
    EXPECT_EQ(wire::load<u32>(message.bytes, 20), 22u);

    auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->txid, 0u);
    EXPECT_EQ(header->ordinal, ours_process::ADD_ARGS.ordinal);

    auto const decoded = ours_process::decode_add_args(message.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->args.count, 3u);
    EXPECT_EQ(read_all(decoded->args), (std::vector<std::string>{"routing_checks", "", "child"}));
}

TEST(OursProcessAddArgs, None) {
    Buffer buffer;
    auto const message = owned(ours_process::encode_add_args(buffer.bytes, {}));
    EXPECT_EQ(message.bytes.size(), 24u);
    auto const decoded = ours_process::decode_add_args(message.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->args.count, 0u);
    EXPECT_TRUE(read_all(decoded->args).empty());
}

TEST(OursProcessAddArgs, LargestFitsOneMessage) {
    Buffer buffer;
    // One string with its NUL filling all that is left after the 24 bytes.
    std::string const largest(wire::MAX_MESSAGE_BYTES - 24 - 1, 'a');
    auto const args = std::array<std::string_view, 1>{largest};
    auto const message = owned(ours_process::encode_add_args(buffer.bytes, args));
    EXPECT_EQ(message.bytes.size(), wire::MAX_MESSAGE_BYTES);
    EXPECT_EQ(read_all(ours_process::decode_add_args(message.view())->args),
              (std::vector<std::string>{largest}));

    std::string const too_large(wire::MAX_MESSAGE_BYTES - 24, 'a');
    auto const too_many = std::array<std::string_view, 1>{too_large};
    EXPECT_EQ(ours_process::encode_add_args(buffer.bytes, too_many).unwrap_err(), Status::InvArgs);
}

TEST(OursProcessAddArgs, EncodeRefusals) {
    Buffer buffer;
    // A NUL inside an argument would end it early on the other side.
    auto const with_nul = std::array<std::string_view, 1>{std::string_view{"a\0b", 3}};
    EXPECT_EQ(ours_process::encode_add_args(buffer.bytes, with_nul).unwrap_err(), Status::InvArgs);
    auto const args = std::array<std::string_view, 1>{"x"};
    EXPECT_EQ(ours_process::encode_add_args(std::span{buffer.bytes}.first(25), args).unwrap_err(),
              Status::BufferTooSmall);
}

TEST(OursProcessAddArgs, DecodeRefusals) {
    Buffer buffer;
    auto const args = std::array<std::string_view, 2>{"ab", "c"};
    auto const good = owned(ours_process::encode_add_args(buffer.bytes, args));
    ASSERT_TRUE(ours_process::decode_add_args(good.view()).is_ok());
    ASSERT_EQ(good.bytes.size(), 24u + 5);

    auto refused = [](Message const message) {
        auto const decoded = ours_process::decode_add_args(message);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };

    // A count that disagrees with the NULs.
    for (auto const count: {u32{0}, u32{1}, u32{3}, u32{0xFFFF'FFFF}}) {
        auto broken = good;
        broken.set_u32(16, count);
        EXPECT_TRUE(refused(broken.view())) << "count " << count;
    }
    // A size that disagrees with the message.
    for (auto const size: {u32{4}, u32{6}, u32{0xFFFF'FFFF}}) {
        auto broken = good;
        broken.set_u32(20, size);
        EXPECT_TRUE(refused(broken.view())) << "size " << size;
    }
    // The last string without its NUL: the message cut by one, size to
    // match, and the NUL still in memory behind the cut.
    auto unterminated = good;
    unterminated.set_u32(20, 4);
    EXPECT_TRUE(refused(unterminated.cut(28)));
    // Bytes after the last NUL: the count of NULs is right, but a third
    // string runs off the end.
    auto trailing = good;
    trailing.bytes.push_back('d');
    trailing.set_u32(20, 6);
    EXPECT_TRUE(refused(trailing.view()));
    // A NUL where a string's byte was: one string more than the count says.
    auto extra_nul = good;
    extra_nul.bytes[24] = 0;
    EXPECT_TRUE(refused(extra_nul.view()));
    // Cut inside the fixed part.
    EXPECT_TRUE(refused(good.cut(23)));
    // No handles.
    auto with_handle = good;
    with_handle.handles.push_back(JOB);
    EXPECT_TRUE(refused(with_handle.view()));
    // One-way: no txid.
    auto with_txid = good;
    with_txid.set_u32(0, 1);
    EXPECT_TRUE(refused(with_txid.view()));
    // A reply is never a request.
    auto reply = good;
    reply.bytes[4] = static_cast<u8>(wire::Direction::ToClient);
    EXPECT_TRUE(refused(reply.view()));
}

// --- Launcher.AddHandles ----------------------------------------------------

TEST(OursProcessAddHandles, RoundTrip) {
    Buffer buffer;
    auto const infos = std::array{
        HandleInfo{.handle = JOB, .id = ours::handle_info(ours::HandleType::JobDefault, 0)},
        HandleInfo{.handle = handle(1), .id = ours::handle_info(ours::HandleType::User0, 7)},
    };
    auto const message =
        owned(ours_process::encode_add_handles(buffer.bytes, buffer.handles, infos));
    EXPECT_EQ(message.bytes.size(), 24u + (2 * 8));
    ASSERT_EQ(message.handles.size(), 2u);
    EXPECT_EQ(wire::load<u32>(message.bytes, 24), wire::HANDLE_PRESENT);
    EXPECT_EQ(wire::load<u32>(message.bytes, 32), wire::HANDLE_PRESENT);

    auto const decoded = ours_process::decode_add_handles(message.view());
    ASSERT_TRUE(decoded.is_ok());
    ASSERT_EQ(decoded->handles.size(), 2u);
    for (usize index = 0; index < infos.size(); ++index) {
        EXPECT_EQ(decoded->at(index).handle, infos[index].handle);
        EXPECT_EQ(decoded->at(index).id, infos[index].id);
    }
}

TEST(OursProcessAddHandles, WhatTheLauncherJudgesStillDecodes) {
    // A reserved type, an NsDir and reserved bits are the launcher's to
    // refuse at Launch, with InvArgs: id is a plain u32 on the wire, so any
    // value decodes.
    Buffer buffer;
    auto const infos = std::array{
        HandleInfo{.handle = handle(0), .id = ours::handle_info(ours::HandleType::VmaRoot, 0)},
        HandleInfo{.handle = handle(1), .id = ours::handle_info(ours::HandleType::NsDir, 0)},
        HandleInfo{.handle = handle(2), .id = 0xFFFF'FFFF},
    };
    auto const message =
        owned(ours_process::encode_add_handles(buffer.bytes, buffer.handles, infos));
    auto const decoded = ours_process::decode_add_handles(message.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->at(2).id, 0xFFFF'FFFFu);
}

TEST(OursProcessAddHandles, AsManyAsOneMessageCarries) {
    Buffer buffer;
    std::vector<HandleInfo> infos;
    for (usize index = 0; index < wire::MAX_MESSAGE_HANDLES; ++index) {
        infos.push_back({.handle = handle(index), .id = 0xF0});
    }
    auto const message =
        owned(ours_process::encode_add_handles(buffer.bytes, buffer.handles, infos));
    EXPECT_EQ(ours_process::decode_add_handles(message.view())->handles.size(),
              wire::MAX_MESSAGE_HANDLES);

    infos.push_back({.handle = handle(64), .id = 0xF0});
    std::vector<Uandle> handles(infos.size());
    EXPECT_EQ(ours_process::encode_add_handles(buffer.bytes, handles, infos).unwrap_err(),
              Status::InvArgs);

    // Built by hand, as no channel would carry it: a 65th entry and handle,
    // with a count to match.
    auto too_many = message;
    too_many.bytes.resize(too_many.bytes.size() + 8);
    too_many.set_u32(24 + (64 * 8), wire::HANDLE_PRESENT);
    too_many.set_u32(16, 65);
    too_many.handles.push_back(handle(64));
    EXPECT_EQ(ours_process::decode_add_handles(too_many.view()).unwrap_err(), Status::InvArgs);
}

TEST(OursProcessAddHandles, EncodeRefusals) {
    Buffer buffer;
    auto const invalid = std::array{HandleInfo{.handle = Uandle::Invalid, .id = 0x03}};
    EXPECT_EQ(ours_process::encode_add_handles(buffer.bytes, buffer.handles, invalid).unwrap_err(),
              Status::InvArgs);
    auto const one = std::array{HandleInfo{.handle = JOB, .id = 0x03}};
    EXPECT_EQ(ours_process::encode_add_handles(buffer.bytes, {}, one).unwrap_err(),
              Status::BufferTooSmall);
    EXPECT_EQ(
        ours_process::encode_add_handles(std::span{buffer.bytes}.first(31), buffer.handles, one)
            .unwrap_err(),
        Status::BufferTooSmall);
}

TEST(OursProcessAddHandles, DecodeRefusals) {
    Buffer buffer;
    auto const infos = std::array{
        HandleInfo{.handle = handle(0), .id = 0x03},
        HandleInfo{.handle = handle(1), .id = 0xF0},
    };
    auto const good = owned(ours_process::encode_add_handles(buffer.bytes, buffer.handles, infos));
    ASSERT_TRUE(ours_process::decode_add_handles(good.view()).is_ok());

    auto refused = [](Message const message) {
        auto const decoded = ours_process::decode_add_handles(message);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };

    // A count that disagrees with the handles, and handles that disagree
    // with the entries.
    for (auto const count: {u32{1}, u32{3}}) {
        auto broken = good;
        broken.set_u32(16, count);
        EXPECT_TRUE(refused(broken.view())) << "count " << count;
    }
    auto one_handle_short = good;
    one_handle_short.handles.pop_back();
    one_handle_short.set_u32(16, 1);
    EXPECT_TRUE(refused(one_handle_short.view()));
    auto one_handle_more = good;
    one_handle_more.handles.push_back(handle(2));
    one_handle_more.set_u32(16, 3);
    EXPECT_TRUE(refused(one_handle_more.view()));
    // The padding is zero.
    auto pad = good;
    pad.set_u32(20, 1);
    EXPECT_TRUE(refused(pad.view()));
    // Cut inside an entry, or one byte too long.
    EXPECT_TRUE(refused(good.cut(36)));
    auto longer = good;
    longer.bytes.push_back(0);
    EXPECT_TRUE(refused(longer.view()));
    // A handle field without HANDLE_PRESENT, and an invalid handle.
    auto absent = good;
    absent.set_u32(32, 0);
    EXPECT_TRUE(refused(absent.view()));
    auto invalid = good;
    invalid.handles[1] = Uandle::Invalid;
    EXPECT_TRUE(refused(invalid.view()));
    // One-way: no txid.
    auto with_txid = good;
    with_txid.set_u32(0, 1);
    EXPECT_TRUE(refused(with_txid.view()));
}

// --- Launcher.AddNames ------------------------------------------------------

TEST(OursProcessAddNames, RoundTrip) {
    Buffer buffer;
    auto const names = std::array{
        NameInfo{.path = "/boot", .directory = handle(0)},
        NameInfo{.path = "/svc", .directory = handle(1)},
    };
    auto const message = owned(ours_process::encode_add_names(buffer.bytes, buffer.handles, names));
    // 24, two handle fields, then "/boot\0" and "/svc\0".
    EXPECT_EQ(message.bytes.size(), 24u + 8 + 6 + 5);
    ASSERT_EQ(message.handles.size(), 2u);
    EXPECT_EQ(wire::load<u32>(message.bytes, 24), wire::HANDLE_PRESENT);
    EXPECT_EQ(wire::load<u32>(message.bytes, 28), wire::HANDLE_PRESENT);

    auto const decoded = ours_process::decode_add_names(message.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(read_all(decoded->paths), (std::vector<std::string>{"/boot", "/svc"}));
    ASSERT_EQ(decoded->directories.size(), 2u);
    EXPECT_EQ(decoded->directories[0], handle(0));
    EXPECT_EQ(decoded->directories[1], handle(1));
}

TEST(OursProcessAddNames, LongestPath) {
    Buffer buffer;
    std::string const longest(ours_process::MAX_PATH_LENGTH, 'p');
    auto const names = std::array{NameInfo{.path = longest, .directory = handle(0)}};
    auto const message = owned(ours_process::encode_add_names(buffer.bytes, buffer.handles, names));
    EXPECT_EQ(read_all(ours_process::decode_add_names(message.view())->paths),
              (std::vector<std::string>{longest}));

    std::string const too_long(ours_process::MAX_PATH_LENGTH + 1, 'p');
    auto const too_long_names = std::array{NameInfo{.path = too_long, .directory = handle(0)}};
    EXPECT_EQ(
        ours_process::encode_add_names(buffer.bytes, buffer.handles, too_long_names).unwrap_err(),
        Status::InvArgs);

    // Encoded by hand: a path one byte too long is malformed, since a
    // string's bound is part of the message's shape, not the server's to
    // judge.
    auto broken = message;
    broken.bytes.insert(broken.bytes.begin() + 28, 'p');
    broken.set_u32(20, static_cast<u32>(ours_process::MAX_PATH_LENGTH + 2));
    EXPECT_EQ(ours_process::decode_add_names(broken.view()).unwrap_err(), Status::InvArgs);
}

TEST(OursProcessAddNames, EncodeRefusals) {
    Buffer buffer;
    auto const invalid = std::array{NameInfo{.path = "/boot", .directory = Uandle::Invalid}};
    EXPECT_EQ(ours_process::encode_add_names(buffer.bytes, buffer.handles, invalid).unwrap_err(),
              Status::InvArgs);
    auto const with_nul =
        std::array{NameInfo{.path = std::string_view{"/a\0b", 4}, .directory = handle(0)}};
    EXPECT_EQ(ours_process::encode_add_names(buffer.bytes, buffer.handles, with_nul).unwrap_err(),
              Status::InvArgs);
    auto const one = std::array{NameInfo{.path = "/boot", .directory = handle(0)}};
    EXPECT_EQ(ours_process::encode_add_names(buffer.bytes, {}, one).unwrap_err(),
              Status::BufferTooSmall);
}

TEST(OursProcessAddNames, DecodeRefusals) {
    Buffer buffer;
    auto const names = std::array{
        NameInfo{.path = "/boot", .directory = handle(0)},
        NameInfo{.path = "/svc", .directory = handle(1)},
    };
    auto const good = owned(ours_process::encode_add_names(buffer.bytes, buffer.handles, names));
    ASSERT_TRUE(ours_process::decode_add_names(good.view()).is_ok());
    ASSERT_EQ(good.bytes.size(), 24u + 8 + 11);

    auto refused = [](Message const message) {
        auto const decoded = ours_process::decode_add_names(message);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };

    // A count that disagrees with the handles or with the paths.
    auto count = good;
    count.set_u32(16, 1);
    EXPECT_TRUE(refused(count.view()));
    auto one_handle_short = good;
    one_handle_short.handles.pop_back();
    EXPECT_TRUE(refused(one_handle_short.view()));
    // A size that disagrees with the message.
    for (auto const size: {u32{10}, u32{12}, u32{0xFFFF'FFFF}}) {
        auto broken = good;
        broken.set_u32(20, size);
        EXPECT_TRUE(refused(broken.view())) << "size " << size;
    }
    // The last path without its NUL.
    auto unterminated = good;
    unterminated.set_u32(20, 10);
    EXPECT_TRUE(refused(unterminated.cut(good.bytes.size() - 1)));
    // Bytes after the last NUL.
    auto trailing = good;
    trailing.bytes.push_back('x');
    trailing.set_u32(20, 12);
    EXPECT_TRUE(refused(trailing.view()));
    // Cut before the paths.
    EXPECT_TRUE(refused(good.cut(31)));
    // A handle field without HANDLE_PRESENT, and an invalid handle.
    auto absent = good;
    absent.set_u32(28, 0);
    EXPECT_TRUE(refused(absent.view()));
    auto invalid = good;
    invalid.handles[0] = Uandle::Invalid;
    EXPECT_TRUE(refused(invalid.view()));
    // One-way: no txid.
    auto with_txid = good;
    with_txid.set_u32(0, 1);
    EXPECT_TRUE(refused(with_txid.view()));
}

// --- Launcher.Launch --------------------------------------------------------

auto launch_request(std::string_view const name) -> LaunchRequest {
    return {
        .txid = 5,
        .info = {.executable = EXECUTABLE, .job = JOB, .name = name},
    };
}

TEST(OursProcessLaunch, RoundTrip) {
    Buffer buffer;
    auto const message =
        owned(ours_process::encode_launch(buffer.bytes, buffer.handles, launch_request("shell")));
    EXPECT_EQ(message.bytes.size(), 28u + 5);
    ASSERT_EQ(message.handles.size(), 2u);
    EXPECT_EQ(message.handles[0], EXECUTABLE);
    EXPECT_EQ(message.handles[1], JOB);

    auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->txid, 5u);
    EXPECT_EQ(header->ordinal, ours_process::LAUNCH.ordinal);

    auto const decoded = ours_process::decode_launch(message.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, 5u);
    EXPECT_EQ(decoded->info.executable, EXECUTABLE);
    EXPECT_EQ(decoded->info.job, JOB);
    EXPECT_EQ(decoded->info.name, "shell");
}

TEST(OursProcessLaunch, Names) {
    // Empty and MAX_NAME_LEN are well formed; a NUL inside is the
    // launcher's to refuse: the name travels with its size, not ended by a
    // NUL, so the codec carries one.
    for (auto const name: {std::string_view{}, std::string_view{"a\0b", 3},
                           std::string_view{"0123456789abcdef0123456789abcdef"}}) {
        Buffer buffer;
        auto const message =
            owned(ours_process::encode_launch(buffer.bytes, buffer.handles, launch_request(name)));
        EXPECT_EQ(ours_process::decode_launch(message.view())->info.name, name);
    }
    Buffer buffer;
    auto const too_long = std::string(ours_process::MAX_NAME_LEN + 1, 'n');
    EXPECT_EQ(ours_process::encode_launch(buffer.bytes, buffer.handles, launch_request(too_long))
                  .unwrap_err(),
              Status::InvArgs);

    // Encoded by hand: a name one byte too long is malformed, since a
    // string's bound is part of the message's shape, not the server's to
    // judge.
    auto longest = owned(
        ours_process::encode_launch(buffer.bytes, buffer.handles,
                                    launch_request(std::string(ours_process::MAX_NAME_LEN, 'n'))));
    longest.bytes.push_back('n');
    longest.set_u32(24, static_cast<u32>(ours_process::MAX_NAME_LEN + 1));
    EXPECT_EQ(ours_process::decode_launch(longest.view()).unwrap_err(), Status::InvArgs);
}

TEST(OursProcessLaunch, EncodeRefusals) {
    Buffer buffer;
    auto no_txid = launch_request("x");
    no_txid.txid = 0;
    EXPECT_EQ(ours_process::encode_launch(buffer.bytes, buffer.handles, no_txid).unwrap_err(),
              Status::InvArgs);
    auto no_job = launch_request("x");
    no_job.info.job = Uandle::Invalid;
    EXPECT_EQ(ours_process::encode_launch(buffer.bytes, buffer.handles, no_job).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(ours_process::encode_launch(buffer.bytes, std::span{buffer.handles}.first(1),
                                          launch_request("x"))
                  .unwrap_err(),
              Status::BufferTooSmall);
}

TEST(OursProcessLaunch, DecodeRefusals) {
    Buffer buffer;
    auto const good =
        owned(ours_process::encode_launch(buffer.bytes, buffer.handles, launch_request("ab")));
    ASSERT_TRUE(ours_process::decode_launch(good.view()).is_ok());

    auto refused = [](Message const message) {
        auto const decoded = ours_process::decode_launch(message);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };

    // A name that runs past the message, stops short of its end, or is
    // longer than any name may be.
    for (auto const name_size: {u32{1}, u32{3}, u32{ours_process::MAX_NAME_LEN + 1}}) {
        auto broken = good;
        broken.set_u32(24, name_size);
        EXPECT_TRUE(refused(broken.view())) << "name_size " << name_size;
    }
    EXPECT_TRUE(refused(good.cut(27)));
    // Two handles, both present and valid.
    auto one_handle = good;
    one_handle.handles.pop_back();
    EXPECT_TRUE(refused(one_handle.view()));
    auto three_handles = good;
    three_handles.handles.push_back(PROCESS);
    EXPECT_TRUE(refused(three_handles.view()));
    for (auto const offset: {usize{16}, usize{20}}) {
        auto absent = good;
        absent.set_u32(offset, 0);
        EXPECT_TRUE(refused(absent.view())) << "offset " << offset;
    }
    auto invalid = good;
    invalid.handles[1] = Uandle::Invalid;
    EXPECT_TRUE(refused(invalid.view()));
    // Two-way: a txid, below the top bit.
    for (auto const txid: {u32{0}, wire::MAX_TXID + 1}) {
        auto broken = good;
        broken.set_u32(0, txid);
        EXPECT_TRUE(refused(broken.view())) << "txid " << txid;
    }
    // Another method's ordinal.
    auto other = good;
    wire::store(std::span{other.bytes}, 8, ours_process::ADD_ARGS.ordinal);
    EXPECT_TRUE(refused(other.view()));
}

TEST(OursProcessLaunchReply, RoundTrip) {
    Buffer buffer;
    auto const ok = owned(ours_process::encode_launch_reply(
        buffer.bytes, buffer.handles, 5, LaunchReply{.status = Status::Ok, .process = PROCESS}));
    EXPECT_EQ(ok.bytes.size(), wire::STATUS_FORM_SIZE);
    ASSERT_EQ(ok.handles.size(), 1u);
    auto const decoded_ok = ours_process::decode_launch_reply(ok.view(), 5);
    ASSERT_TRUE(decoded_ok.is_ok());
    EXPECT_EQ(decoded_ok->status, Status::Ok);
    EXPECT_EQ(decoded_ok->process, PROCESS);

    Buffer failed_buffer;
    auto const failed = owned(ours_process::encode_launch_reply(
        failed_buffer.bytes, failed_buffer.handles, 5, LaunchReply{.status = Status::InvArgs}));
    EXPECT_EQ(failed.bytes.size(), wire::STATUS_FORM_SIZE);
    EXPECT_TRUE(failed.handles.empty());
    auto const decoded_failed = ours_process::decode_launch_reply(failed.view(), 5);
    ASSERT_TRUE(decoded_failed.is_ok());
    EXPECT_EQ(decoded_failed->status, Status::InvArgs);
    EXPECT_EQ(decoded_failed->process, Uandle::Invalid);
}

TEST(OursProcessLaunchReply, Refusals) {
    Buffer buffer;
    // No process next to a failure, and none missing next to Ok.
    EXPECT_EQ(ours_process::encode_launch_reply(
                  buffer.bytes, buffer.handles, 5,
                  LaunchReply{.status = Status::AccessDenied, .process = PROCESS})
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(ours_process::encode_launch_reply(buffer.bytes, buffer.handles, 5,
                                                LaunchReply{.status = Status::Ok})
                  .unwrap_err(),
              Status::InvArgs);

    auto const good = owned(ours_process::encode_launch_reply(
        buffer.bytes, buffer.handles, 5, LaunchReply{.status = Status::Ok, .process = PROCESS}));
    auto refused = [](Message const message, u32 const txid) {
        auto const decoded = ours_process::decode_launch_reply(message, txid);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };
    // Another request's reply.
    EXPECT_TRUE(refused(good.view(), 6));
    // Ok without its process, or with one more.
    auto no_handle = good;
    no_handle.handles.clear();
    EXPECT_TRUE(refused(no_handle.view(), 5));
    auto two_handles = good;
    two_handles.handles.push_back(JOB);
    EXPECT_TRUE(refused(two_handles.view(), 5));
    auto absent = good;
    absent.set_u32(20, 0);
    EXPECT_TRUE(refused(absent.view(), 5));
    // A failure that carries a process.
    auto failure_with_process = good;
    failure_with_process.set_u32(16, static_cast<u32>(Status::InvArgs));
    EXPECT_TRUE(refused(failure_with_process.view(), 5));
    // A request is never a reply.
    auto request = good;
    request.bytes[4] = static_cast<u8>(wire::Direction::ToServer);
    EXPECT_TRUE(refused(request.view(), 5));
}

// --- The protocol -----------------------------------------------------------

TEST(OursProcess, Methods) {
    // The kinds, which no hash checks; the ordinals are checked by
    // wire/test/ordinals_test.py.
    EXPECT_EQ(ours_process::ADD_ARGS.kind, wire::MethodKind::OneWay);
    EXPECT_EQ(ours_process::ADD_HANDLES.kind, wire::MethodKind::OneWay);
    EXPECT_EQ(ours_process::ADD_NAMES.kind, wire::MethodKind::OneWay);
    EXPECT_EQ(ours_process::LAUNCH.kind, wire::MethodKind::TwoWay);
    EXPECT_EQ(ours_process::LAUNCHER_NAME, "ours.process.Launcher");
}
} // namespace
