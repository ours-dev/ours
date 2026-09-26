/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The processargs encoder (<ours/process_args.hpp>) and decoder
/// (runtime::ProcessArgs) against each other, against Zircon's own golden
/// bytes, and against messages broken one check at a time.
#include <runtime/process_args.hpp>
#include <runtime/startup_handles.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

namespace {
using ours::HandleType;
using ours::Status;

/// Zircon's process_builder golden message (src/lib/process_builder/src/
/// process_args.rs, the build_and_write_message test): three handles --
/// User1 with arg 0x1234, and NsDir 0 and 1 -- three arguments, one
/// environment variable and two names. The layout is the same byte for
/// byte; only the protocol and version words are ours.
constexpr u8 GOLDEN[] = {
    0x4f, 0x50, 0x41, 0x52, // protocol, "OPAR"
    0x01, 0x00, 0x00, 0x00, // version
    0x24, 0x00, 0x00, 0x00, // handle_info_off
    0x30, 0x00, 0x00, 0x00, // args_off
    0x03, 0x00, 0x00, 0x00, // args_num
    0x3f, 0x00, 0x00, 0x00, // environ_off
    0x01, 0x00, 0x00, 0x00, // environ_num
    0x47, 0x00, 0x00, 0x00, // names_off
    0x02, 0x00, 0x00, 0x00, // names_num
    0xf1, 0x00, 0x34, 0x12, // handle_info: User1, 0x1234
    0x20, 0x00, 0x00, 0x00, //              NsDir, 0
    0x20, 0x00, 0x01, 0x00, //              NsDir, 1
    'a',  'r',  'g',  '1',  0,   'a', 'r', 'g', '2', 0,   'a', 'r', 'g', '3', 0,   'F', 'O',
    'O',  '=',  'B',  'A',  'R', 0,   '/', 'd', 'a', 't', 'a', 0,   '/', 'p', 'k', 'g', 0,
};

constexpr u32 GOLDEN_INFO[] = {
    ours::handle_info(HandleType::User1, 0x1234),
    ours::handle_info(HandleType::NsDir, 0),
    ours::handle_info(HandleType::NsDir, 1),
};
constexpr std::string_view GOLDEN_ARGS[] = {"arg1", "arg2", "arg3"};
constexpr std::string_view GOLDEN_ENVIRON[] = {"FOO=BAR"};
constexpr std::string_view GOLDEN_NAMES[] = {"/data", "/pkg"};

auto encode_golden(std::span<u8> const out, u32 *const length) -> Status {
    return ours::encode_process_args(out, GOLDEN_INFO, GOLDEN_ARGS, GOLDEN_ENVIRON, GOLDEN_NAMES,
                                     length);
}

auto strings(runtime::ProcessArgs::Strings const run) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    for (auto const string: run) {
        out.push_back(string);
    }
    return out;
}

/// The golden message, as a mutable copy to break.
auto golden() -> std::vector<u8> {
    return {std::begin(GOLDEN), std::end(GOLDEN)};
}

auto put_u32(std::vector<u8> &message, usize const at, u32 const value) -> void {
    std::memcpy(message.data() + at, &value, sizeof(value));
}

/// Encodes into a buffer filled with a sentinel and answers the status,
/// failing the test if a refusal wrote anything: encode_process_args()
/// promises to leave |out| and |length| alone unless it succeeds.
template <usize N>
auto refused(std::span<u32 const> const infos, std::span<std::string_view const> const args)
    -> Status {
    std::array<u8, N> buffer;
    buffer.fill(0xa5);
    u32 length = 0xdead;
    auto const status = ours::encode_process_args(buffer, infos, args, {}, {}, &length);
    if (status != Status::Ok) {
        EXPECT_EQ(length, 0xdeadu);
        for (auto const byte: buffer) {
            EXPECT_EQ(byte, 0xa5);
            if (byte != 0xa5) {
                break;
            }
        }
    }
    return status;
}

auto parses(std::span<u8 const> const message, u32 const num_handles) -> Status {
    runtime::ProcessArgs view;
    return runtime::ProcessArgs::parse(message, num_handles, &view);
}

TEST(HandleInfo, PacksTypeLowAndArgHigh) {
    constexpr auto INFO = ours::handle_info(HandleType::NsDir, 0xbeef);
    static_assert(INFO == 0xbeef'0020);
    static_assert(ours::handle_info_type(INFO) == HandleType::NsDir);
    static_assert(ours::handle_info_arg(INFO) == 0xbeef);
}

TEST(Encode, WritesZirconsLayoutByteForByte) {
    std::array<u8, ours::CHANNEL_MAX_MSG_BYTES> buffer{};
    u32 length = 0;
    ASSERT_EQ(encode_golden(buffer, &length), Status::Ok);
    ASSERT_EQ(length, sizeof(GOLDEN));
    EXPECT_EQ(std::memcmp(buffer.data(), GOLDEN, sizeof(GOLDEN)), 0);
}

TEST(Encode, AnEmptyMessageIsTheHeaderAlone) {
    std::array<u8, sizeof(ours::ProcArgs)> buffer{};
    u32 length = 0;
    ASSERT_EQ(ours::encode_process_args(buffer, {}, {}, {}, {}, &length), Status::Ok);
    EXPECT_EQ(length, sizeof(ours::ProcArgs));

    runtime::ProcessArgs view;
    ASSERT_EQ(runtime::ProcessArgs::parse({buffer.data(), length}, 0, &view), Status::Ok);
    EXPECT_EQ(view.num_handles(), 0u);
    EXPECT_EQ(view.args().size(), 0u);
    EXPECT_EQ(view.args().begin(), view.args().end());
    EXPECT_EQ(view.names().size(), 0u);
}

TEST(Encode, RefusesAStringWithANulInside) {
    // After a good one, so a check that ran only once writing had begun
    // would have written the header already.
    constexpr std::string_view SPLIT[] = {"fine", std::string_view("a\0b", 3)};
    EXPECT_EQ(refused<128>({}, SPLIT), Status::InvArgs);
}

TEST(Encode, RefusesReservedHandleInfoBits) {
    constexpr u32 INFO[] = {0x0000'0120};
    EXPECT_EQ(refused<128>(INFO, {}), Status::InvArgs);
}

TEST(Encode, RefusesWhatNoChannelMessageCouldCarry) {
    std::vector<u8> buffer(2 * ours::CHANNEL_MAX_MSG_BYTES);
    u32 length = 0;

    std::vector<u32> infos(ours::CHANNEL_MAX_MSG_HANDLES + 1,
                           ours::handle_info(HandleType::User0, 0));
    EXPECT_EQ(refused<2 * ours::CHANNEL_MAX_MSG_BYTES>(infos, {}), Status::OutOfRange);
    infos.pop_back();
    EXPECT_EQ(ours::encode_process_args(buffer, infos, {}, {}, {}, &length), Status::Ok);

    // One byte over: the header and a string whose NUL is the 4097th byte.
    std::string const long_arg(ours::CHANNEL_MAX_MSG_BYTES - sizeof(ours::ProcArgs), 'x');
    std::string_view const over[] = {long_arg};
    EXPECT_EQ(refused<2 * ours::CHANNEL_MAX_MSG_BYTES>({}, over), Status::OutOfRange);
    std::string_view const exact[] = {std::string_view(long_arg).substr(1)};
    ASSERT_EQ(ours::encode_process_args(buffer, {}, exact, {}, {}, &length), Status::Ok);
    EXPECT_EQ(length, ours::CHANNEL_MAX_MSG_BYTES);
}

TEST(Encode, BufferTooSmallWritesNothing) {
    std::array<u8, sizeof(GOLDEN) - 1> buffer{};
    buffer.fill(0xa5);
    u32 length = 0xdead;
    EXPECT_EQ(encode_golden(buffer, &length), Status::BufferTooSmall);
    EXPECT_EQ(length, 0xdeadu);
    for (auto const byte: buffer) {
        ASSERT_EQ(byte, 0xa5);
    }
}

TEST(Parse, ReadsBackWhatWasWritten) {
    runtime::ProcessArgs view;
    ASSERT_EQ(runtime::ProcessArgs::parse(GOLDEN, 3, &view), Status::Ok);
    ASSERT_EQ(view.num_handles(), 3u);
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_EQ(view.handle_info(i), GOLDEN_INFO[i]) << "handle " << i;
    }
    EXPECT_EQ(strings(view.args()), (std::vector<std::string_view>{"arg1", "arg2", "arg3"}));
    EXPECT_EQ(strings(view.environ()), (std::vector<std::string_view>{"FOO=BAR"}));
    EXPECT_EQ(strings(view.names()), (std::vector<std::string_view>{"/data", "/pkg"}));

    // Each string is also a C string: its NUL is right after it.
    for (auto const arg: view.args()) {
        EXPECT_EQ(arg.data()[arg.size()], '\0');
    }
}

TEST(Parse, NeedsTheWholeHeader) {
    EXPECT_EQ(parses({GOLDEN, sizeof(ours::ProcArgs) - 1}, 0), Status::InvArgs);
}

TEST(Parse, NeedsOurProtocolAndVersion) {
    auto message = golden();
    put_u32(message, offsetof(ours::ProcArgs, protocol), 0x4150'585d); // Zircon's
    EXPECT_EQ(parses(message, 3), Status::InvArgs);

    message = golden();
    put_u32(message, offsetof(ours::ProcArgs, version), ours::PROCARGS_VERSION + 1);
    EXPECT_EQ(parses(message, 3), Status::InvArgs);
}

TEST(Parse, BoundsTheHandleInfoArray) {
    auto message = golden();
    put_u32(message, offsetof(ours::ProcArgs, handle_info_off), sizeof(ours::ProcArgs) - 4);
    EXPECT_EQ(parses(message, 3), Status::InvArgs) << "inside the header";

    message = golden();
    put_u32(message, offsetof(ours::ProcArgs, handle_info_off), sizeof(ours::ProcArgs) + 1);
    EXPECT_EQ(parses(message, 3), Status::InvArgs) << "not on a 4-byte boundary";

    // Past the end, on a 4-byte boundary so that the alignment check is not
    // what refuses it: without the bound, `bytes - off` wraps and the array
    // is read from beyond the message.
    for (u32 const past: {84u, 0x1000u}) {
        message = golden();
        put_u32(message, offsetof(ours::ProcArgs, handle_info_off), past);
        EXPECT_EQ(parses(message, 3), Status::InvArgs) << "past the end at " << past;
    }

    // More handles than entries: the golden message has room for exactly
    // the three it describes before its strings are read as handle_infos,
    // so bound the message itself instead.
    std::array<u8, sizeof(ours::ProcArgs) + 4> small{};
    u32 length = 0;
    constexpr u32 ONE[] = {ours::handle_info(HandleType::User0, 0)};
    ASSERT_EQ(ours::encode_process_args(small, ONE, {}, {}, {}, &length), Status::Ok);
    EXPECT_EQ(parses(small, 1), Status::Ok);
    EXPECT_EQ(parses(small, 2), Status::InvArgs);
    // The case Zircon's newer ld::ProcessargsBuffer::Valid() lets through:
    // 40 bytes, handle_info at 36, five handles claimed.
    EXPECT_EQ(parses(small, 5), Status::InvArgs);
}

TEST(Parse, BoundsEachStringSection) {
    for (auto const [off_field, num_field]: {
             std::pair{offsetof(ours::ProcArgs, args_off), offsetof(ours::ProcArgs, args_num)},
             std::pair{offsetof(ours::ProcArgs, environ_off),
                       offsetof(ours::ProcArgs, environ_num)},
             std::pair{offsetof(ours::ProcArgs, names_off), offsetof(ours::ProcArgs, names_num)},
         }) {
        auto message = golden();
        put_u32(message, off_field, sizeof(ours::ProcArgs) - 1);
        EXPECT_EQ(parses(message, 3), Status::InvArgs) << "inside the header, field " << off_field;

        message = golden();
        put_u32(message, off_field, static_cast<u32>(message.size()));
        EXPECT_EQ(parses(message, 3), Status::InvArgs) << "at the end, field " << off_field;

        // Past the end: `num > bytes - off` alone would wrap and pass.
        for (auto const past: {static_cast<u32>(message.size() + 1), u32{0x1000}}) {
            message = golden();
            put_u32(message, off_field, past);
            EXPECT_EQ(parses(message, 3), Status::InvArgs)
                << "past the end at " << past << ", field " << off_field;
        }

        message = golden();
        put_u32(message, num_field, static_cast<u32>(message.size()));
        EXPECT_EQ(parses(message, 3), Status::InvArgs) << "too many, field " << num_field;
    }
}

TEST(Parse, NeedsEveryStringToEndInsideTheMessage) {
    // Drop the final NUL: the last name, "/pkg", now runs off the end.
    EXPECT_EQ(parses({GOLDEN, sizeof(GOLDEN) - 1}, 3), Status::InvArgs);

    // Claim seven arguments. The six strings after args_off -- the three
    // arguments, the variable and the two names -- all end in bounds, so
    // only the seventh, which would start at the very end, is missing.
    auto message = golden();
    put_u32(message, offsetof(ours::ProcArgs, args_num), 7);
    EXPECT_EQ(parses(message, 3), Status::InvArgs);
}

TEST(Parse, RefusesReservedHandleInfoBitsButNotUnknownTypes) {
    auto message = golden();
    put_u32(message, sizeof(ours::ProcArgs), 0x0000'0120);
    EXPECT_EQ(parses(message, 3), Status::InvArgs);

    message = golden();
    put_u32(message, sizeof(ours::ProcArgs), 0x0000'00e7); // no such type
    EXPECT_EQ(parses(message, 3), Status::Ok);
}

TEST(StartupHandles, TakesByTypeAndArgOnce) {
    std::array<ours::HandleValue, 3> handles{0x11, 0x23, 0x37};
    std::array<u32, 3> infos{GOLDEN_INFO[0], GOLDEN_INFO[1], GOLDEN_INFO[2]};
    runtime::StartupHandles table(handles, infos);

    EXPECT_EQ(table.take(HandleType::NsDir, 1), 0x37u);
    EXPECT_EQ(table.take(HandleType::NsDir, 1), ours::HANDLE_INVALID) << "taken already";
    EXPECT_EQ(table.take(HandleType::User1, 0), ours::HANDLE_INVALID) << "wrong arg";
    EXPECT_EQ(table.take(HandleType::User1, 0x1234), 0x11u);
    EXPECT_EQ(table.take(HandleType::JobDefault, 0), ours::HANDLE_INVALID) << "never there";
    EXPECT_EQ(table.take(HandleType::NsDir, 0), 0x23u);
    EXPECT_EQ(handles, (std::array<ours::HandleValue, 3>{}));
}
} // namespace
