/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The Launcher's per-connection state (<launch/process_launcher.hpp>): the
// client side is the real codec, the start hook a fake that records what it
// is handed, and the processargs that would reach the program is read back
// with the runtime's own decoder. Every handle a test gives the state must
// come out exactly once, closed or handed to the hook.
#include <launch/process_launcher.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <ort/process_args.hpp>

namespace {
using launch::LauncherOps;
using launch::ProcessLauncherState;
using launch::Startup;
using ours::HandleType;
using ours::Status;
using ours::Uandle;
using ours_process::HandleInfo;
using ours_process::NameInfo;
using Outcome = ProcessLauncherState::Outcome;
using Received = ProcessLauncherState::Received;

constexpr auto PROCESS = Uandle{0x7003};
constexpr auto TXID = u32{11};

/// What the fake start hook was handed, copied out of the call.
struct StartCall {
    Uandle job;
    std::string name;
    Uandle executable;
    std::vector<u8> process_args;
    std::vector<Uandle> handles;
};

/// The system calls, faked: the start hook records its call and answers
/// |answer|; close records the handle.
struct Fake {
    std::vector<StartCall> started;
    std::vector<Uandle> closed;
    Status answer = Status::Ok;

    static auto start(void *const context, Startup const &startup) -> ustl::Result<Uandle, Status> {
        auto &fake = *static_cast<Fake *>(context);
        fake.started.push_back(StartCall{
            .job = startup.job,
            .name = std::string(startup.name),
            .executable = startup.executable,
            .process_args = {startup.process_args.begin(), startup.process_args.end()},
            .handles = {startup.handles.begin(), startup.handles.end()},
        });
        if (fake.answer != Status::Ok) {
            return ustl::err(fake.answer);
        }
        return ustl::ok(PROCESS);
    }

    static auto close(void *const context, Uandle const handle) -> void {
        static_cast<Fake *>(context)->closed.push_back(handle);
    }
};

/// A client of one connection: encodes each request with the codec and
/// hands it to the state, and gives every handle it sends a value no other
/// handle of the test has.
struct Client {
    Fake fake;
    ProcessLauncherState state{
        LauncherOps{.start = &Fake::start, .close = &Fake::close, .context = &fake}};
    std::vector<u8> bytes = std::vector<u8>(wire::MAX_MESSAGE_BYTES);
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> handles{};
    std::vector<u8> scratch = std::vector<u8>(ours::CHANNEL_MAX_MSG_BYTES);
    std::vector<Uandle> given;
    u32 next = 0x1003;

    auto make() -> Uandle {
        auto const handle = static_cast<Uandle>(next);
        next += 0x100;
        given.push_back(handle);
        return handle;
    }

    auto send(ustl::Result<wire::Message, Status> const &encoded) -> Received {
        EXPECT_TRUE(encoded.is_ok());
        return state.receive(encoded.unwrap(), scratch);
    }

    auto add_args(std::vector<std::string_view> const &args) -> Received {
        return send(ours_process::encode_add_args(bytes, args));
    }

    /// AddHandles with |ids|, a new handle each.
    auto add_handles(std::vector<u32> const &ids) -> Received {
        std::vector<HandleInfo> infos;
        for (auto const id: ids) {
            infos.push_back({.handle = make(), .id = id});
        }
        return send(ours_process::encode_add_handles(bytes, handles, infos));
    }

    /// AddNames with |paths|, a new directory each.
    auto add_names(std::vector<std::string_view> const &paths) -> Received {
        std::vector<NameInfo> names;
        for (auto const path: paths) {
            names.push_back({.path = path, .directory = make()});
        }
        return send(ours_process::encode_add_names(bytes, handles, names));
    }

    Uandle executable = Uandle::Invalid;
    Uandle job = Uandle::Invalid;

    auto launch(std::string_view const name = "program") -> Received {
        executable = make();
        job = make();
        return send(ours_process::encode_launch(
            bytes, handles,
            {.txid = TXID, .info = {.executable = executable, .job = job, .name = name}}));
    }

    /// Every handle given out came back exactly once: closed, or handed to
    /// the start hook.
    auto expect_accounted() const -> void {
        auto back = fake.closed;
        for (auto const &call: fake.started) {
            back.insert(back.end(), call.handles.begin(), call.handles.end());
        }
        auto sorted_given = given;
        std::ranges::sort(sorted_given);
        std::ranges::sort(back);
        EXPECT_EQ(back, sorted_given);
    }
};

auto user0(u16 const arg = 0) -> u32 {
    return ours::handle_info(HandleType::User0, arg);
}

/// The processargs of |call|, read back as the program would, with
/// |num_handles| handles.
auto parse(StartCall const &call) -> ort::ProcessArgs {
    ort::ProcessArgs parsed;
    EXPECT_EQ(ort::ProcessArgs::parse(call.process_args, static_cast<u32>(call.handles.size() + 2),
                                      &parsed),
              Status::Ok);
    return parsed;
}

auto strings(ort::ProcessArgs::Strings const strings) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (auto const string: strings) {
        out.emplace_back(string);
    }
    return out;
}

auto expect_reply(Received const &received, Status const status) -> void {
    EXPECT_EQ(received.outcome, Outcome::Reply);
    EXPECT_EQ(received.txid, TXID);
    EXPECT_EQ(received.reply.status, status);
    if (status == Status::Ok) {
        EXPECT_EQ(received.reply.process, PROCESS);
    } else {
        EXPECT_EQ(received.reply.process, Uandle::Invalid);
    }
}

TEST(ProcessLauncherState, LaunchHandsEverythingToStart) {
    Client client;
    EXPECT_EQ(client.add_args({"routing_checks", "child"}).outcome, Outcome::Continue);
    EXPECT_EQ(client.add_handles({ours::handle_info(HandleType::JobDefault, 0), user0(7)}).outcome,
              Outcome::Continue);
    EXPECT_EQ(client.add_names({"/boot", "/svc"}).outcome, Outcome::Continue);
    // Calling an Add* again appends to what the last call gave.
    EXPECT_EQ(client.add_args({"-v"}).outcome, Outcome::Continue);
    EXPECT_TRUE(client.fake.closed.empty());

    expect_reply(client.launch("routing_checks"), Status::Ok);
    ASSERT_EQ(client.fake.started.size(), 1u);
    auto const &call = client.fake.started[0];
    EXPECT_EQ(call.name, "routing_checks");
    EXPECT_EQ(call.executable, client.executable);
    EXPECT_EQ(call.job, client.job);
    // The handles in the order they came, AddHandles' and AddNames' alike.
    EXPECT_EQ(call.handles, (std::vector<Uandle>(client.given.begin(), client.given.begin() + 4)));

    auto const parsed = parse(call);
    EXPECT_EQ(strings(parsed.args()), (std::vector<std::string>{"routing_checks", "child", "-v"}));
    EXPECT_EQ(parsed.environ().size(), 0u);
    EXPECT_EQ(strings(parsed.names()), (std::vector<std::string>{"/boot", "/svc"}));
    ASSERT_EQ(parsed.num_handles(), 6u);
    EXPECT_EQ(parsed.handle_info(0), ours::handle_info(HandleType::JobDefault, 0));
    EXPECT_EQ(parsed.handle_info(1), user0(7));
    // Each NsDir's argument is its path's index among the names.
    EXPECT_EQ(parsed.handle_info(2), ours::handle_info(HandleType::NsDir, 0));
    EXPECT_EQ(parsed.handle_info(3), ours::handle_info(HandleType::NsDir, 1));
    // The launcher's own, after the client's.
    EXPECT_EQ(parsed.handle_info(4), ours::handle_info(HandleType::VmaRoot, 0));
    EXPECT_EQ(parsed.handle_info(5), ours::handle_info(HandleType::VmoVdso, 0));

    // The job and the program are the state's to close; the rest went to
    // the hook.
    EXPECT_EQ(client.fake.closed, (std::vector<Uandle>{client.executable, client.job}));
    client.expect_accounted();
}

TEST(ProcessLauncherState, ResetsAfterLaunch) {
    Client client;
    ASSERT_EQ(client.add_args({"first"}).outcome, Outcome::Continue);
    ASSERT_EQ(client.add_handles({user0()}).outcome, Outcome::Continue);
    ASSERT_EQ(client.add_names({"/boot"}).outcome, Outcome::Continue);
    expect_reply(client.launch(), Status::Ok);

    // The next Launch carries nothing of the first.
    expect_reply(client.launch(), Status::Ok);
    ASSERT_EQ(client.fake.started.size(), 2u);
    auto const &second = client.fake.started[1];
    EXPECT_TRUE(second.handles.empty());
    auto const parsed = parse(second);
    EXPECT_EQ(parsed.args().size(), 0u);
    EXPECT_EQ(parsed.names().size(), 0u);
    ASSERT_EQ(parsed.num_handles(), 2u);
    EXPECT_EQ(parsed.handle_info(0), ours::handle_info(HandleType::VmaRoot, 0));
    client.expect_accounted();
}

TEST(ProcessLauncherState, ResetsAfterAFailedLaunch) {
    // A mark lasts until the Launch it fails, and no further.
    Client client;
    std::vector<std::string_view> const too_many(ProcessLauncherState::MAX_ARGS + 1, "a");
    ASSERT_EQ(client.add_args(too_many).outcome, Outcome::Continue);
    expect_reply(client.launch(), Status::InvArgs);
    EXPECT_TRUE(client.fake.started.empty());

    ASSERT_EQ(client.add_args({"again"}).outcome, Outcome::Continue);
    expect_reply(client.launch(), Status::Ok);
    ASSERT_EQ(client.fake.started.size(), 1u);
    EXPECT_EQ(strings(parse(client.fake.started[0]).args()), (std::vector<std::string>{"again"}));
    client.expect_accounted();
}

TEST(ProcessLauncherState, SixtyTwoHandlesFitAndSixtyThreeDoNot) {
    // 62: a processargs carries 64 handles, and the launcher adds two.
    static_assert(ProcessLauncherState::MAX_HANDLES == 62);
    {
        Client client;
        ASSERT_EQ(client.add_handles(std::vector<u32>(31, user0())).outcome, Outcome::Continue);
        ASSERT_EQ(client.add_handles(std::vector<u32>(30, user0())).outcome, Outcome::Continue);
        ASSERT_EQ(client.add_names({"/boot"}).outcome, Outcome::Continue);
        expect_reply(client.launch(), Status::Ok);
        ASSERT_EQ(client.fake.started.size(), 1u);
        EXPECT_EQ(client.fake.started[0].handles.size(), 62u);
        EXPECT_EQ(parse(client.fake.started[0]).num_handles(), 64u);
        client.expect_accounted();
    }
    {
        // The 63rd in a message of its own: that message's handle is
        // closed at once, and Launch answers InvArgs.
        Client client;
        ASSERT_EQ(client.add_handles(std::vector<u32>(62, user0())).outcome, Outcome::Continue);
        ASSERT_EQ(client.add_handles({user0()}).outcome, Outcome::Continue);
        EXPECT_EQ(client.fake.closed, (std::vector<Uandle>{client.given.back()}));
        expect_reply(client.launch(), Status::InvArgs);
        EXPECT_TRUE(client.fake.started.empty());
        client.expect_accounted();
    }
    {
        // As a namespace entry.
        Client client;
        ASSERT_EQ(client.add_handles(std::vector<u32>(62, user0())).outcome, Outcome::Continue);
        ASSERT_EQ(client.add_names({"/boot"}).outcome, Outcome::Continue);
        expect_reply(client.launch(), Status::InvArgs);
        EXPECT_TRUE(client.fake.started.empty());
        client.expect_accounted();
    }
    {
        // All 63 in one message.
        Client client;
        ASSERT_EQ(client.add_handles(std::vector<u32>(63, user0())).outcome, Outcome::Continue);
        expect_reply(client.launch(), Status::InvArgs);
        EXPECT_TRUE(client.fake.started.empty());
        client.expect_accounted();
    }
}

TEST(ProcessLauncherState, StringBytesFitTheProcessargs) {
    // With no handle of the client's, the strings may take all a
    // processargs leaves after its header and the launcher's two
    // handle_info: the message comes out at exactly the channel's limit.
    static_assert(ProcessLauncherState::MAX_STRING_BYTES == 4096 - 36 - 8);
    auto const most = std::string(ProcessLauncherState::MAX_STRING_BYTES - 1, 'a');
    {
        Client client;
        ASSERT_EQ(client.add_args({most}).outcome, Outcome::Continue);
        expect_reply(client.launch(), Status::Ok);
        ASSERT_EQ(client.fake.started.size(), 1u);
        EXPECT_EQ(client.fake.started[0].process_args.size(), ours::CHANNEL_MAX_MSG_BYTES);
        EXPECT_EQ(strings(parse(client.fake.started[0]).args()), (std::vector<std::string>{most}));
    }
    {
        // One byte more does not fit the state.
        Client client;
        ASSERT_EQ(client.add_args({most + "a"}).outcome, Outcome::Continue);
        expect_reply(client.launch(), Status::InvArgs);
        EXPECT_TRUE(client.fake.started.empty());
    }
    {
        // Split across two AddArgs and a path, one byte more in all: the
        // second AddArgs is the one that does not fit. Each string takes
        // its bytes and its NUL.
        Client client;
        constexpr auto TOTAL = ProcessLauncherState::MAX_STRING_BYTES + 1;
        auto const first = std::string((TOTAL / 2) - 1, 'a');
        auto const path = std::string_view{"/b"};
        auto const second = std::string(TOTAL - (first.size() + 1) - (path.size() + 1) - 1, 'c');
        ASSERT_EQ(client.add_args({first}).outcome, Outcome::Continue);
        ASSERT_EQ(client.add_names({path}).outcome, Outcome::Continue);
        ASSERT_EQ(client.add_args({second}).outcome, Outcome::Continue);
        expect_reply(client.launch(), Status::InvArgs);
        EXPECT_TRUE(client.fake.started.empty());
        client.expect_accounted();
    }
    {
        // A path that does not fit the arena the arguments filled: AddNames
        // checks the same arena, so its directory is closed at once rather
        // than kept, and its path is never copied past the arena's end.
        Client client;
        ASSERT_EQ(client.add_args({most}).outcome, Outcome::Continue);
        ASSERT_EQ(client.add_names({"/boot"}).outcome, Outcome::Continue);
        EXPECT_EQ(client.fake.closed, (std::vector<Uandle>{client.given.back()}));
        expect_reply(client.launch(), Status::InvArgs);
        EXPECT_TRUE(client.fake.started.empty());
        client.expect_accounted();
    }
}

TEST(ProcessLauncherState, HandlesAndBytesShareTheMessage) {
    // Each handle a client adds takes four bytes of the same processargs,
    // which only Launch can count: the strings fit the state, but not
    // beside the handle_info.
    constexpr auto HANDLES = usize{10};
    auto const fits = std::string(ProcessLauncherState::MAX_STRING_BYTES - (4 * HANDLES) - 1, 'a');
    {
        Client client;
        ASSERT_EQ(client.add_handles(std::vector<u32>(HANDLES, user0())).outcome,
                  Outcome::Continue);
        ASSERT_EQ(client.add_args({fits}).outcome, Outcome::Continue);
        expect_reply(client.launch(), Status::Ok);
        ASSERT_EQ(client.fake.started.size(), 1u);
        EXPECT_EQ(client.fake.started[0].process_args.size(), ours::CHANNEL_MAX_MSG_BYTES);
        client.expect_accounted();
    }
    {
        Client client;
        ASSERT_EQ(client.add_handles(std::vector<u32>(HANDLES, user0())).outcome,
                  Outcome::Continue);
        ASSERT_EQ(client.add_args({fits + "a"}).outcome, Outcome::Continue);
        EXPECT_TRUE(client.fake.closed.empty());
        expect_reply(client.launch(), Status::InvArgs);
        EXPECT_TRUE(client.fake.started.empty());
        // The handles were never handed on, so the state closed them.
        client.expect_accounted();
    }
}

TEST(ProcessLauncherState, ArgumentCount) {
    {
        Client client;
        std::vector<std::string_view> const most(ProcessLauncherState::MAX_ARGS, "");
        ASSERT_EQ(client.add_args(most).outcome, Outcome::Continue);
        expect_reply(client.launch(), Status::Ok);
        ASSERT_EQ(client.fake.started.size(), 1u);
        EXPECT_EQ(parse(client.fake.started[0]).args().size(), ProcessLauncherState::MAX_ARGS);
    }
    {
        Client client;
        std::vector<std::string_view> const most(ProcessLauncherState::MAX_ARGS, "a");
        ASSERT_EQ(client.add_args(most).outcome, Outcome::Continue);
        ASSERT_EQ(client.add_args({"one more"}).outcome, Outcome::Continue);
        expect_reply(client.launch(), Status::InvArgs);
        EXPECT_TRUE(client.fake.started.empty());
    }
}

TEST(ProcessLauncherState, ReservedHandleTypesAreRefused) {
    // What the launcher adds itself, or what describes its own work, and
    // NsDir, which needs a path.
    auto const refused = std::array{
        ours::handle_info(HandleType::ProcSelf, 0), ours::handle_info(HandleType::ThreadSelf, 0),
        ours::handle_info(HandleType::VmaRoot, 0),  ours::handle_info(HandleType::VmaLoaded, 0),
        ours::handle_info(HandleType::VmoStack, 0), ours::handle_info(HandleType::VmoExecutable, 0),
        ours::handle_info(HandleType::NsDir, 0),    user0() | 0x0100,
    };
    for (auto const id: refused) {
        Client client;
        // All of the message or none of it: the good handle beside the
        // refused one is closed too, at once.
        ASSERT_EQ(client.add_handles({user0(), id}).outcome, Outcome::Continue);
        EXPECT_EQ(client.fake.closed, client.given) << "id " << id;
        expect_reply(client.launch(), Status::InvArgs);
        EXPECT_TRUE(client.fake.started.empty()) << "id " << id;
        client.expect_accounted();
    }
}

TEST(ProcessLauncherState, OtherHandleTypesPass) {
    Client client;
    ASSERT_EQ(client
                  .add_handles({ours::handle_info(HandleType::JobDefault, 0),
                                ours::handle_info(HandleType::DirectoryRequest, 0),
                                ours::handle_info(HandleType::Resource, 0), user0(3)})
                  .outcome,
              Outcome::Continue);
    expect_reply(client.launch(), Status::Ok);
    ASSERT_EQ(client.fake.started.size(), 1u);
    EXPECT_EQ(client.fake.started[0].handles.size(), 4u);
    client.expect_accounted();
}

TEST(ProcessLauncherState, NameWithNulIsRefused) {
    Client client;
    ASSERT_EQ(client.add_handles({user0()}).outcome, Outcome::Continue);
    expect_reply(client.launch(std::string_view{"a\0b", 3}), Status::InvArgs);
    EXPECT_TRUE(client.fake.started.empty());
    client.expect_accounted();
}

TEST(ProcessLauncherState, StartFailureIsTheAnswer) {
    Client client;
    client.fake.answer = Status::AccessDenied;
    ASSERT_EQ(client.add_handles({user0()}).outcome, Outcome::Continue);
    expect_reply(client.launch(), Status::AccessDenied);
    ASSERT_EQ(client.fake.started.size(), 1u);
    // The hook took the client's handle, whatever it answered: the state
    // closed only the job and the program.
    EXPECT_EQ(client.fake.closed, (std::vector<Uandle>{client.executable, client.job}));
    client.expect_accounted();
}

TEST(ProcessLauncherState, MalformedMessagesCloseTheConnection) {
    // Each case: something held, then a message that is not a Launcher
    // request. The state closes what it held and what the message brought.
    auto expect_close = [](Client &client, wire::Message const message) {
        auto const received = client.state.receive(message, client.scratch);
        EXPECT_EQ(received.outcome, Outcome::Close);
        EXPECT_TRUE(client.fake.started.empty());
        client.expect_accounted();
    };

    {
        // A malformed AddHandles, with a handle of its own.
        Client client;
        ASSERT_EQ(client.add_handles({user0()}).outcome, Outcome::Continue);
        auto const brought = client.make();
        auto encoded = ours_process::encode_add_handles(
            client.bytes, client.handles, std::array{HandleInfo{.handle = brought, .id = user0()}});
        ASSERT_TRUE(encoded.is_ok());
        wire::store(std::span{client.bytes}, 20, u32{1});
        expect_close(client, encoded.unwrap());
    }
    {
        // An ordinal Launcher does not have.
        Client client;
        ASSERT_EQ(client.add_names({"/boot"}).outcome, Outcome::Continue);
        auto encoded = ours_process::encode_add_args(client.bytes, {});
        ASSERT_TRUE(encoded.is_ok());
        wire::store(std::span{client.bytes}, 8, u64{0x1234'5678});
        expect_close(client, encoded.unwrap());
    }
    {
        // A reply, as a client that handed the root both ends of one
        // channel would make it read its own answer.
        Client client;
        ASSERT_EQ(client.add_handles({user0()}).outcome, Outcome::Continue);
        auto const process = client.make();
        auto encoded = ours_process::encode_launch_reply(
            client.bytes, client.handles, TXID, {.status = Status::Ok, .process = process});
        expect_close(client, encoded.unwrap());
    }
    {
        // A Launch without a txid, carrying its two handles.
        Client client;
        auto const executable = client.make();
        auto const job = client.make();
        auto encoded = ours_process::encode_launch(
            client.bytes, client.handles,
            {.txid = TXID, .info = {.executable = executable, .job = job, .name = "x"}});
        ASSERT_TRUE(encoded.is_ok());
        wire::store(std::span{client.bytes}, 0, u32{0});
        expect_close(client, encoded.unwrap());
    }
    {
        // Shorter than a header.
        Client client;
        ASSERT_EQ(client.add_args({"x"}).outcome, Outcome::Continue);
        auto const bytes = std::array<u8, 8>{};
        expect_close(client, wire::Message{.bytes = bytes, .handles = {}});
    }
}

TEST(ProcessLauncherState, ResetClosesWhatIsHeld) {
    Client client;
    ASSERT_EQ(client.add_handles({user0(), user0()}).outcome, Outcome::Continue);
    ASSERT_EQ(client.add_names({"/boot"}).outcome, Outcome::Continue);
    EXPECT_TRUE(client.fake.closed.empty());
    // The connection went away.
    client.state.reset();
    client.expect_accounted();
    // Nothing is closed twice when the state goes.
    client.state.reset();
    client.expect_accounted();
}

/// What the state constant_state below calls.
Fake CONSTANT_FAKE;

/// A state kept the way the root keeps its Launcher connections
/// (component-framework step 14): constant-initialised, its hooks fixed at
/// compile time, and never destroyed. The declaration is half the test:
/// constinit refuses to compile unless the constructor is constexpr.
[[clang::no_destroy]]
constinit ProcessLauncherState CONSTANT_STATE{
    LauncherOps{.start = &Fake::start, .close = &Fake::close, .context = &CONSTANT_FAKE}};

TEST(ProcessLauncherState, AConstinitStateServesLikeAnyOther) {
    // Its hooks are the ones it was initialised with: the handle it is given
    // is closed through them when the connection goes away.
    std::vector<u8> bytes(wire::MAX_MESSAGE_BYTES);
    std::array<Uandle, 1> handles{};
    auto const given = Uandle{0x1003};
    auto encoded = ours_process::encode_add_handles(
        bytes, handles, std::array{HandleInfo{.handle = given, .id = user0()}});
    ASSERT_TRUE(encoded.is_ok());
    std::vector<u8> scratch(ours::CHANNEL_MAX_MSG_BYTES);
    ASSERT_EQ(CONSTANT_STATE.receive(encoded.unwrap(), scratch).outcome, Outcome::Continue);
    EXPECT_TRUE(CONSTANT_FAKE.closed.empty());
    CONSTANT_STATE.reset();
    EXPECT_EQ(CONSTANT_FAKE.closed, std::vector{given});
}

TEST(ProcessLauncherState, DestructionClosesWhatIsHeld) {
    Fake fake;
    std::vector<Uandle> given;
    {
        ProcessLauncherState state{
            LauncherOps{.start = &Fake::start, .close = &Fake::close, .context = &fake}};
        std::vector<u8> bytes(wire::MAX_MESSAGE_BYTES);
        std::array<Uandle, 1> handles{};
        given.push_back(Uandle{0x1003});
        auto encoded = ours_process::encode_add_handles(
            bytes, handles, std::array{HandleInfo{.handle = given[0], .id = user0()}});
        ASSERT_TRUE(encoded.is_ok());
        std::vector<u8> scratch(ours::CHANNEL_MAX_MSG_BYTES);
        ASSERT_EQ(state.receive(encoded.unwrap(), scratch).outcome, Outcome::Continue);
        EXPECT_TRUE(fake.closed.empty());
    }
    EXPECT_EQ(fake.closed, given);
}
} // namespace
