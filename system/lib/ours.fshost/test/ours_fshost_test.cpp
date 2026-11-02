/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The admin codec: the Mount request written and read back, its refusals,
// and the reply's status form.
#include <ours/status.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <ours_fshost/ours_fshost.hpp>

namespace {
using ours::Status;
using ours_fshost::MountRequest;

constexpr auto NAME = std::string_view{"extra"};

/// A handles array with the directory handle in it, what a caller holds.
auto handles_with(ours::Uandle handle) -> std::array<ours::Uandle, 1> {
    return {handle};
}

TEST(FshostMount, RoundTripsNameAndHandle) {
    auto bytes = std::array<u8, 128>{};
    auto handles = handles_with(ours::Uandle{7});
    auto const encoded = ours_fshost::encode_mount(
        bytes, handles, {.txid = 4, .handle = ours::Uandle{7}, .name = NAME});
    ASSERT_TRUE(encoded.is_ok());
    EXPECT_EQ(encoded->bytes.size(), 20 + NAME.size());
    EXPECT_EQ(encoded->handles.size(), usize{1});

    auto const decoded = ours_fshost::decode_mount(encoded.unwrap());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, u32{4});
    EXPECT_EQ(decoded->name, NAME);
}

TEST(FshostMount, RefusesEmptyAndOversizedNames) {
    auto bytes = std::array<u8, 512>{};
    auto handles = handles_with(ours::Uandle{1});
    auto const empty = ours_fshost::encode_mount(
        bytes, handles, {.txid = 1, .handle = ours::Uandle{1}, .name = std::string_view{}});
    EXPECT_EQ(empty.unwrap_err(), Status::InvArgs);

    // 255 x's: exactly the cap, then one past it.
    auto const long_name = std::string_view{"xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"
                                            "xxxxx"};
    EXPECT_EQ(long_name.size(), usize{255});
    auto const at_cap = ours_fshost::encode_mount(
        bytes, handles, {.txid = 1, .handle = ours::Uandle{1}, .name = long_name});
    EXPECT_TRUE(at_cap.is_ok());
    auto const one_more = std::string{long_name} + "x";
    auto const oversized = ours_fshost::encode_mount(
        bytes, handles, {.txid = 1, .handle = ours::Uandle{1}, .name = one_more});
    EXPECT_EQ(oversized.unwrap_err(), Status::InvArgs);
}

TEST(FshostMount, RefusesAMissingDirectoryHandle) {
    auto bytes = std::array<u8, 128>{};
    auto no_handles = std::array<ours::Uandle, 0>{};
    auto const encoded = ours_fshost::encode_mount(
        bytes, no_handles, {.txid = 1, .handle = ours::Uandle{1}, .name = NAME});
    EXPECT_EQ(encoded.unwrap_err(), Status::InvArgs);
}

TEST(FshostMount, ReplyIsTheStatusForm) {
    auto bytes = std::array<u8, wire::STATUS_FORM_SIZE>{};
    auto const encoded = ours_fshost::encode_mount_reply(bytes, 9, Status::Ok);
    ASSERT_TRUE(encoded.is_ok());
    auto const decoded = wire::decode_reply_status(encoded.unwrap(), ours_fshost::MOUNT, 9);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded.unwrap(), Status::Ok);
}
} // namespace
