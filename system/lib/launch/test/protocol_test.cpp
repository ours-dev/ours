/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The launcher service's Launch request (<launch/protocol.hpp>): what
// encode_launch() writes, decode_launch() reads back, and every malformed
// message decode_launch() must refuse -- the launcher reads requests from
// programs it does not trust.
#include <launch/protocol.hpp>

#include <array>
#include <string_view>
#include <vector>

#include <cstring>

#include <gtest/gtest.h>
#include <ours/process_args.hpp>
#include <ours/syscall.hpp>

namespace {
using launch::protocol::decode_launch;
using launch::protocol::encode_launch;
using launch::protocol::Header;
using launch::protocol::Launch;
using launch::protocol::MAX_PROGRAM_NAME;
using launch::protocol::Ordinal;
using launch::protocol::ordinal_of;
using ours::HandleType;
using ours::Status;

/// A Launch for `bin/fault_probe none` with one startup handle (User0).
auto encoded(std::vector<u8> &buffer) -> u32 {
    buffer.assign(ours::CHANNEL_MAX_MSG_BYTES, 0xCC);
    std::array<std::string_view, 2> const args{"bin/fault_probe", "none"};
    std::array<std::string_view, 1> const environ{"ours.test=1"};
    std::array<u32, 1> const infos{ours::handle_info(HandleType::User0, 0)};
    u32 length = 0;
    EXPECT_EQ(encode_launch(buffer, "bin/fault_probe", infos, args, environ, {}, &length),
              Status::Ok);
    buffer.resize(length);
    return length;
}

auto strings(ort::ProcessArgs::Strings const run) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    for (auto const string: run) {
        out.push_back(string);
    }
    return out;
}

TEST(LaunchProtocol, RoundTrip) {
    std::vector<u8> message;
    encoded(message);

    Launch launch{};
    ASSERT_EQ(decode_launch(message, 2, &launch), Status::Ok);
    EXPECT_EQ(launch.name, "bin/fault_probe");
    EXPECT_EQ(launch.parsed.num_handles(), 1u);
    EXPECT_EQ(launch.parsed.handle_info(0), ours::handle_info(HandleType::User0, 0));
    EXPECT_EQ(strings(launch.parsed.args()),
              (std::vector<std::string_view>{"bin/fault_probe", "none"}));
    EXPECT_EQ(strings(launch.parsed.environ()), (std::vector<std::string_view>{"ours.test=1"}));

    // The processargs part starts on a 4-byte boundary, after zero padding.
    auto const offset = static_cast<usize>(launch.process_args.data() - message.data());
    EXPECT_EQ(offset % 4, 0u);
    for (auto at = sizeof(Header) + launch.name.size(); at < offset; ++at) {
        EXPECT_EQ(message[at], 0) << "padding byte " << at;
    }

    auto const ordinal = ordinal_of(message);
    ASSERT_TRUE(ordinal.is_some());
    EXPECT_EQ(*ordinal, Ordinal::Launch);
}

TEST(LaunchProtocol, EncodeRefusals) {
    std::vector<u8> buffer(ours::CHANNEL_MAX_MSG_BYTES);
    u32 length = 0;
    std::array<std::string_view, 1> const args{"x"};

    EXPECT_EQ(encode_launch(buffer, "", {}, args, {}, {}, &length), Status::InvArgs);
    std::string const long_name(MAX_PROGRAM_NAME + 1, 'a');
    EXPECT_EQ(encode_launch(buffer, long_name, {}, args, {}, {}, &length), Status::InvArgs);

    // The launcher adds VmaRoot; a request that names one is refused.
    std::array<u32, 1> const root{ours::handle_info(HandleType::VmaRoot, 0)};
    EXPECT_EQ(encode_launch(buffer, "bin/x", root, args, {}, {}, &length), Status::InvArgs);

    // The job takes one of the message's handles.
    std::vector<u32> const too_many(ours::CHANNEL_MAX_MSG_HANDLES,
                                    ours::handle_info(HandleType::User0, 0));
    EXPECT_EQ(encode_launch(buffer, "bin/x", too_many, args, {}, {}, &length), Status::OutOfRange);

    std::vector<std::string_view> const too_many_args(launch::protocol::MAX_STRINGS + 1, "x");
    EXPECT_EQ(encode_launch(buffer, "bin/x", {}, too_many_args, {}, {}, &length),
              Status::OutOfRange);

    std::vector<u8> tiny(12);
    EXPECT_NE(encode_launch(tiny, "bin/x", {}, args, {}, {}, &length), Status::Ok);
}

TEST(LaunchProtocol, DecodeRefusals) {
    std::vector<u8> good;
    encoded(good);
    Launch launch{};

    // No job.
    EXPECT_EQ(decode_launch(good, 0, &launch), Status::InvArgs);
    // A handle count the processargs does not describe.
    EXPECT_EQ(decode_launch(good, 3, &launch), Status::InvArgs);

    // Shorter than a header, and cut anywhere after it.
    for (usize size = 0; size < good.size(); ++size) {
        EXPECT_EQ(decode_launch({good.data(), size}, 2, &launch), Status::InvArgs)
            << "cut at " << size;
    }

    auto with_header = [&](Header const header) {
        auto message = good;
        std::memcpy(message.data(), &header, sizeof(header));
        return message;
    };
    auto const name_size = static_cast<u32>(std::string_view("bin/fault_probe").size());
    EXPECT_EQ(decode_launch(with_header({Ordinal::Connect, name_size}), 2, &launch),
              Status::InvArgs);
    EXPECT_EQ(decode_launch(with_header({static_cast<Ordinal>(0), name_size}), 2, &launch),
              Status::InvArgs);
    EXPECT_EQ(decode_launch(with_header({Ordinal::Launch, 0}), 2, &launch), Status::InvArgs);
    EXPECT_EQ(decode_launch(with_header({Ordinal::Launch, MAX_PROGRAM_NAME + 1}), 2, &launch),
              Status::InvArgs);
    EXPECT_EQ(decode_launch(with_header({Ordinal::Launch, 0xFFFF'FFFF}), 2, &launch),
              Status::InvArgs);

    // A VmaRoot slipped into the processargs.
    auto forged = good;
    ASSERT_EQ(decode_launch(forged, 2, &launch), Status::Ok);
    auto const offset = static_cast<usize>(launch.process_args.data() - forged.data());
    ours::ProcArgs header{};
    std::memcpy(&header, forged.data() + offset, sizeof(header));
    auto const root = ours::handle_info(HandleType::VmaRoot, 0);
    std::memcpy(forged.data() + offset + header.handle_info_off, &root, sizeof(root));
    EXPECT_EQ(decode_launch(forged, 2, &launch), Status::InvArgs);

    EXPECT_FALSE(ordinal_of({good.data(), sizeof(Header) - 1}).is_some());
}
} // namespace
