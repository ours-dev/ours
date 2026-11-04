/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <loop/loop.hpp>

#include <array>
#include <functional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "fake_kernel.hpp"
#include <gtest/gtest.h>
#include <ours/port.hpp>
#include <ours/syscall.hpp>

// How gtest prints a value it shows in a failure, found by argument-dependent
// lookup in the value's own namespace: a status by name, a key as its index
// and generation, rather than as raw bytes. gtest looks for exactly the name
// PrintTo, hence the NOLINTs.
namespace ours {
// NOLINTNEXTLINE(readability-identifier-naming): gtest's customisation point.
auto PrintTo(Status const status, std::ostream *const out) -> void {
    *out << to_string(status);
}
} // namespace ours

namespace loop {
// NOLINTNEXTLINE(readability-identifier-naming): gtest's customisation point.
auto PrintTo(Key const key, std::ostream *const out) -> void {
    *out << "Key{index " << key.index() << ", generation " << key.generation() << "}";
}
} // namespace loop

// The Loop against the fake kernel of fake_kernel.hpp. The tests named Add*
// and Loop* check add() and the Loop's own lifetime; the tests named Run* pin
// down Loop::run(), the dispatch P3 reserves for the user to write by hand.
// They were written against a stub that answered NotSupported, and are the
// failing tests that exercise starts from.
namespace {
using loop::Action;
using loop::Key;
using ours::Signals;
using ours::Status;
using ours::Uandle;

constexpr auto MESSAGES = Signals::Readable | Signals::PeerClosed;

// A slot is {handle 4, signals 4, Waiter pointer 8, generation 4, flags 1 and
// 3 of padding}: 24 bytes on a target with 8-byte pointers, which every target
// of the project has. The flags byte sits in what was 4 bytes of padding after
// the generation, so the slot that can be suspended is no bigger than the one
// that could not. The function and the context it held before the Waiter, two
// pointers, made it 32. The root's table is LOOP_SLOTS of them
// (system/service/init/pools.hpp), so a byte added to a slot is paid that
// many times; this fails if one is.
static_assert(sizeof(void *) == 8);
static_assert(sizeof(loop::Slot) == 24);

/// What a client's on_signals() answers.
enum class Answer : u8 {
    /// Read one message and answer Rearm; once nothing is left to read and
    /// the peer is gone, answer Close. What a connection's on_signals() does.
    Serve,

    /// Answer Close at once, whatever was observed and with any message
    /// unread.
    Close,

    /// Read one message, if one is queued, and answer Suspend: a connection
    /// that took a request whose reply is not ready.
    Suspend,
};

/// One slot's Waiter: what its on_signals() does and what it saw.
struct Client final: loop::Waiter {
    char name = '?';
    Uandle handle = Uandle::Invalid;
    Answer answer = Answer::Serve;

    /// Run inside on_signals() before it answers, when set.
    std::function<void()> during;

    /// Run inside on_rearm_failed(), when set.
    std::function<void()> during_rearm_failed;

    /// Where every on_signals() appends its client's name, in call order, and
    /// every on_rearm_failed() a '!' and then the name.
    std::string *log = nullptr;

    /// The observed signals of each on_signals() call.
    std::vector<Signals> observed;

    /// The cause of each on_rearm_failed() call.
    std::vector<Status> rearm_failures;

    /// What the fake kernel showed when on_rearm_failed() ran: whether the
    /// client's handle was still open, and how many handle_close calls had
    /// been made. The Loop is to have closed the handle by then.
    bool open_when_told = true;
    usize closes_when_told = 0;

    auto on_rearm_failed(Status const cause) -> void override {
        log->push_back('!');
        log->push_back(name);
        rearm_failures.push_back(cause);
        open_when_told = fake::kernel().is_open(handle);
        closes_when_told = fake::kernel().closes().size();
        if (during_rearm_failed) {
            during_rearm_failed();
        }
    }

    auto on_signals(Signals const signals) -> Action override {
        log->push_back(name);
        observed.push_back(signals);
        if (during) {
            during();
        }
        if (answer == Answer::Close) {
            return Action::Close;
        }
        if (answer == Answer::Suspend) {
            if ((signals & Signals::Readable) != Signals::None) {
                auto _ = fake::kernel().read(handle);
            }
            return Action::Suspend;
        }
        if ((signals & Signals::Readable) != Signals::None) {
            // One message per call (I4), whatever else is queued.
            auto _ = fake::kernel().read(handle);
            return Action::Rearm;
        }
        return Action::Close;
    }
};

/// A packet that looks like one of the Loop's own waits fired with |key|.
auto signal_packet(Key const key) -> ours::PortPacket {
    auto packet = ours::PortPacket{};
    packet.key = key.raw();
    packet.type = ours::PacketType::SignalOne;
    packet.status = Status::Ok;
    packet.signal = ours::PacketSignal{MESSAGES, Signals::Readable | Signals::Writable, 1, 0, 0};
    return packet;
}

/// A User packet with |key|, as port_queue would deliver it.
auto user_packet(Key const key) -> ours::PortPacket {
    auto packet = ours::PortPacket{};
    packet.key = key.raw();
    packet.type = ours::PacketType::User;
    packet.status = Status::Ok;
    return packet;
}

class LoopTest: public ::testing::Test {
  protected:
    static constexpr auto SLOTS = usize{4};

    LoopTest() : m_port{fresh_port()}, m_loop{m_port, m_slots} {}

    /// Resets the fake before anything of this test exists.
    static auto fresh_port() -> Uandle {
        fake::kernel().reset();
        return fake::kernel().make_port();
    }

    /// Gives |client| a new channel end and adds it, waiting for |signals|.
    auto add(Client &client, char const name, Signals const signals = MESSAGES) -> Key {
        client.name = name;
        client.handle = fake::kernel().make_object();
        client.log = &m_log;
        auto added = m_loop.add(client.handle, signals, client);
        EXPECT_TRUE(added.is_ok());
        if (added.is_err()) {
            return Key{0xFFFF'FFFF, 0xFFFF'FFFF};
        }
        return added.unwrap();
    }

    /// Two clients, a and b, each with a message queued, and behind their
    /// packets a stale one that carries a's key. The next object_wait_async,
    /// which is a's re-arm, answers |cause| and arms nothing. Runs the Loop
    /// dry, and checks that a was told once with |cause|, after its handle
    /// was closed and its slot freed, and that nothing else was: not b, and
    /// not a again.
    auto check_failed_rearm_is_told(Status const cause) -> void {
        Client a;
        Client b;
        auto const key = add(a, 'a');
        add(b, 'b');
        fake::kernel().write(a.handle, 1);
        fake::kernel().write(b.handle, 1);
        fake::kernel().queue(m_port, signal_packet(key));
        fake::kernel().fail_next_wait_async(cause);

        EXPECT_EQ(m_loop.run(0), Status::TimeOut);

        // a is called, then told; b is called and re-armed; the stale packet
        // reaches no one.
        EXPECT_EQ(m_log, "a!ab");
        EXPECT_EQ(a.observed.size(), 1U);
        EXPECT_EQ(a.rearm_failures, (std::vector<Status>{cause}));
        EXPECT_TRUE(b.rearm_failures.empty());
        // The handle was closed once, before a was told, and by the Loop.
        EXPECT_FALSE(a.open_when_told);
        EXPECT_EQ(a.closes_when_told, 1U);
        auto const &closes = fake::kernel().closes();
        ASSERT_EQ(closes.size(), 1U);
        EXPECT_EQ(closes[0].handle, a.handle);
        EXPECT_EQ(closes[0].status, Status::Ok);
        // Two adds, then a's re-arm, which failed, and b's.
        auto const &waits = fake::kernel().waits();
        ASSERT_EQ(waits.size(), 4U);
        EXPECT_EQ(waits[2].handle, a.handle);
        EXPECT_EQ(waits[2].status, cause);
        EXPECT_EQ(waits[3].handle, b.handle);
        EXPECT_EQ(waits[3].status, Status::Ok);
        EXPECT_EQ(fake::kernel().queued_on(m_port), 0U);
        // The slot is free, in a new life.
        Client c;
        EXPECT_EQ(add(c, 'c'), (Key{0, 1}));
    }

    /// Adds |client|, has it take one message and answer Suspend, and checks
    /// what a Suspend is: the slot in use, its handle open, the Waiter called
    /// once, and nothing else done -- no wait armed, no handle closed. Returns
    /// the slot's key. The client keeps the answer Suspend; a test changes it
    /// before the slot is resumed to have the next call do something else.
    auto add_suspended(Client &client, char const name) -> Key {
        client.answer = Answer::Suspend;
        auto const key = add(client, name);
        auto const waits_before = fake::kernel().waits().size();
        auto const closes_before = fake::kernel().closes().size();
        auto const calls_before = client.observed.size();
        fake::kernel().write(client.handle, 1);
        EXPECT_EQ(m_loop.run(0), Status::TimeOut);
        EXPECT_EQ(client.observed.size(), calls_before + 1);
        EXPECT_EQ(fake::kernel().waits().size(), waits_before);
        EXPECT_EQ(fake::kernel().armed_on(client.handle), 0U);
        EXPECT_EQ(fake::kernel().queued_on(m_port), 0U);
        EXPECT_TRUE(fake::kernel().is_open(client.handle));
        EXPECT_EQ(fake::kernel().closes().size(), closes_before);
        return key;
    }

    std::array<loop::Slot, SLOTS> m_slots{};
    Uandle m_port;
    loop::Loop m_loop;
    std::string m_log;
};

// --- add() and the Loop's own lifetime. ---

TEST_F(LoopTest, AddHandsOutTheLowestFreeSlotAtItsGeneration) {
    Client a;
    Client b;
    EXPECT_EQ(add(a, 'a'), (Key{0, 0}));
    EXPECT_EQ(add(b, 'b'), (Key{1, 0}));
}

TEST_F(LoopTest, AddArmsOneWaitUnderTheSlotsKey) {
    Client a;
    auto const key = add(a, 'a');
    auto const &waits = fake::kernel().waits();
    ASSERT_EQ(waits.size(), 1U);
    EXPECT_EQ(waits[0].handle, a.handle);
    EXPECT_EQ(waits[0].port, m_port);
    EXPECT_EQ(waits[0].key, key.raw());
    EXPECT_EQ(waits[0].signals, MESSAGES);
    EXPECT_EQ(waits[0].status, Status::Ok);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 1U);
    // Adding is not calling.
    EXPECT_TRUE(m_log.empty());
}

TEST_F(LoopTest, AddRefusesAnInvalidHandleOrNoSignals) {
    auto const handle = fake::kernel().make_object();
    Client client;
    client.log = &m_log;
    auto const no_handle = m_loop.add(Uandle::Invalid, MESSAGES, client);
    ASSERT_TRUE(no_handle.is_err());
    EXPECT_EQ(no_handle.unwrap_err(), Status::InvArgs);
    auto const no_signals = m_loop.add(handle, Signals::None, client);
    ASSERT_TRUE(no_signals.is_err());
    EXPECT_EQ(no_signals.unwrap_err(), Status::InvArgs);
    EXPECT_TRUE(fake::kernel().waits().empty());
    EXPECT_TRUE(fake::kernel().is_open(handle));
    // Nothing was taken: the first slot is still the lowest free one.
    Client first;
    EXPECT_EQ(add(first, 'f'), (Key{0, 0}));
}

TEST_F(LoopTest, AddAnswersNoResourceWhenEverySlotIsInUse) {
    std::array<Client, SLOTS> clients;
    for (auto &client: clients) {
        add(client, 'x');
    }
    auto const spare = fake::kernel().make_object();
    Client client;
    auto const full = m_loop.add(spare, MESSAGES, client);
    ASSERT_TRUE(full.is_err());
    EXPECT_EQ(full.unwrap_err(), Status::NoResource);
    EXPECT_TRUE(fake::kernel().is_open(spare));
    EXPECT_EQ(fake::kernel().waits().size(), SLOTS);
}

TEST_F(LoopTest, AddLeavesTheSlotFreeAndTheHandleWithTheCallerWhenTheWaitFails) {
    auto const handle = fake::kernel().make_object();
    Client client;
    fake::kernel().fail_next_wait_async(Status::AccessDenied);
    auto const refused = m_loop.add(handle, MESSAGES, client);
    ASSERT_TRUE(refused.is_err());
    EXPECT_EQ(refused.unwrap_err(), Status::AccessDenied);
    EXPECT_TRUE(fake::kernel().is_open(handle));
    EXPECT_TRUE(fake::kernel().closes().empty());
    // No key was handed out, so the slot is taken again in the same life.
    Client a;
    EXPECT_EQ(add(a, 'a'), (Key{0, 0}));
}

// A failed add() is answered to its caller, who keeps the handle: the Waiter
// is not told, whatever the reason, and is never called.
TEST_F(LoopTest, AddDoesNotTellTheWaiterWhenItFails) {
    Client refused;
    refused.log = &m_log;
    auto const handle = fake::kernel().make_object();
    fake::kernel().fail_next_wait_async(Status::NoMemory);
    ASSERT_TRUE(m_loop.add(handle, MESSAGES, refused).is_err());
    ASSERT_TRUE(m_loop.add(Uandle::Invalid, MESSAGES, refused).is_err());

    // The table full: NoResource.
    std::array<Client, SLOTS> clients;
    for (auto &client: clients) {
        add(client, 'x');
    }
    auto const full = m_loop.add(handle, MESSAGES, refused);
    ASSERT_TRUE(full.is_err());
    EXPECT_EQ(full.unwrap_err(), Status::NoResource);

    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_TRUE(refused.rearm_failures.empty());
    EXPECT_TRUE(refused.observed.empty());
    EXPECT_TRUE(m_log.empty());
}

TEST(Loop, ClosesItsHandlesAndThenItsPortWhenItGoes) {
    fake::kernel().reset();
    auto const port = fake::kernel().make_port();
    auto const first = fake::kernel().make_object();
    auto const second = fake::kernel().make_object();
    // The Loop does not own its Waiters, which outlive it.
    Client one;
    Client two;
    {
        std::array<loop::Slot, 2> slots{};
        loop::Loop scoped{port, slots};
        auto const added_one = scoped.add(first, MESSAGES, one);
        auto const added_two = scoped.add(second, MESSAGES, two);
        ASSERT_TRUE(added_one.is_ok() && added_two.is_ok());
    }
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 3U);
    EXPECT_EQ(closes[0].handle, first);
    EXPECT_EQ(closes[1].handle, second);
    EXPECT_EQ(closes[2].handle, port);
    for (auto const &close: closes) {
        EXPECT_EQ(close.status, Status::Ok);
    }
}

// The Loop's destructor frees every slot in use, but it is not a failed
// re-arm: the Waiters' owners go with the Loop, and no Waiter is called or
// told.
TEST(Loop, DoesNotCallOrTellItsWaitersWhenItGoes) {
    fake::kernel().reset();
    auto const port = fake::kernel().make_port();
    auto log = std::string{};
    Client one;
    Client two;
    one.log = &log;
    two.log = &log;
    {
        std::array<loop::Slot, 2> slots{};
        loop::Loop scoped{port, slots};
        ASSERT_TRUE(scoped.add(fake::kernel().make_object(), MESSAGES, one).is_ok());
        ASSERT_TRUE(scoped.add(fake::kernel().make_object(), MESSAGES, two).is_ok());
    }
    EXPECT_TRUE(log.empty());
    EXPECT_TRUE(one.rearm_failures.empty());
    EXPECT_TRUE(two.rearm_failures.empty());
    EXPECT_TRUE(one.observed.empty());
    EXPECT_TRUE(two.observed.empty());
}

// --- run(): the function P3 reserves for the user. Each fails on a stub. ---

// Output: TimeOut is how run(0) ends, after one port_wait with deadline 0.
TEST_F(LoopTest, RunReturnsTimeOutOnceThePortIsEmpty) {
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(fake::kernel().deadlines(), (std::vector<ours::Instant>{0}));
}

// Input: the deadline goes to every port_wait unchanged. The fake answers
// BadState where a real INSTANT_INFINITE wait would block.
TEST_F(LoopTest, RunWaitsWithTheDeadlineItIsGiven) {
    Client a;
    add(a, 'a');
    fake::kernel().write(a.handle, 1);
    EXPECT_EQ(m_loop.run(ours::INSTANT_INFINITE), Status::BadState);
    EXPECT_EQ(m_log, "a");
    EXPECT_EQ(fake::kernel().deadlines(),
              (std::vector<ours::Instant>{ours::INSTANT_INFINITE, ours::INSTANT_INFINITE}));
}

// Output: any answer of port_wait but Ok ends run() with that answer.
TEST(LoopRun, ReturnsTheAnswerOfAPortWaitThatFails) {
    fake::kernel().reset();
    std::array<loop::Slot, 1> slots{};
    {
        loop::Loop not_a_port{fake::kernel().make_object(), slots};
        EXPECT_EQ(not_a_port.run(0), Status::BadType);
    }
    {
        loop::Loop no_port{static_cast<Uandle>(0x7FFF'FFFF), slots};
        EXPECT_EQ(no_port.run(0), Status::BadHandle);
    }
}

// Step 2: the Waiter of the slot whose wait fired is called, once, with the
// packet's observed signals; no other Waiter.
TEST_F(LoopTest, RunCallsTheWaiterOfTheSlotWhoseWaitFired) {
    Client a;
    Client b;
    add(a, 'a');
    add(b, 'b');
    fake::kernel().write(b.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "b");
    EXPECT_EQ(b.observed, (std::vector<Signals>{Signals::Readable | Signals::Writable}));
    EXPECT_TRUE(a.observed.empty());
}

// Step 3: Rearm arms the same wait again -- same handle, key and signals --
// and closes nothing.
TEST_F(LoopTest, RunRearmsUnderTheSameKeyAndSignals) {
    Client a;
    auto const key = add(a, 'a');
    fake::kernel().write(a.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "a");
    auto const &waits = fake::kernel().waits();
    ASSERT_EQ(waits.size(), 2U);
    EXPECT_EQ(waits[1].handle, a.handle);
    EXPECT_EQ(waits[1].port, m_port);
    EXPECT_EQ(waits[1].key, key.raw());
    EXPECT_EQ(waits[1].signals, MESSAGES);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 1U);
    EXPECT_TRUE(fake::kernel().closes().empty());
    EXPECT_TRUE(fake::kernel().is_open(a.handle));
}

// I4: one on_signals() per packet, then re-arm. a has three messages and b one;
// a's re-armed wait queues behind b's packet each time, so b is served
// second, not last.
TEST_F(LoopTest, RunServesABusyClientInTurns) {
    Client a;
    Client b;
    add(a, 'a');
    add(b, 'b');
    fake::kernel().write(a.handle, 3);
    fake::kernel().write(b.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "abaa");
}

// Step 4 for Close: the handle is closed once, nothing is armed for it, and
// the slot is free in a new life. The Waiter is told what the object asserted,
// whatever it is: here the peer going away.
TEST_F(LoopTest, RunClosesTheSlotWhenItsWaiterAnswersClose) {
    Client a;
    add(a, 'a');
    fake::kernel().close_peer(a.handle);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "a");
    EXPECT_EQ(a.observed, (std::vector<Signals>{Signals::PeerClosed}));
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, a.handle);
    EXPECT_EQ(closes[0].status, Status::Ok);
    EXPECT_EQ(fake::kernel().waits().size(), 1U);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 0U);
    Client b;
    EXPECT_EQ(add(b, 'b'), (Key{0, 1}));
}

// Step 4 once more: the Loop obeys the answer and does not infer it from what
// was observed. A Waiter that answers Close with its message unread has its
// handle closed, is not called again, and nothing is armed for it.
TEST_F(LoopTest, RunClosesTheSlotWhenItsWaiterAnswersCloseWithAMessageUnread) {
    Client a;
    a.answer = Answer::Close;
    add(a, 'a');
    fake::kernel().write(a.handle, 2);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "a");
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, a.handle);
    EXPECT_EQ(closes[0].status, Status::Ok);
    EXPECT_EQ(fake::kernel().waits().size(), 1U);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 0U);
    Client b;
    EXPECT_EQ(add(b, 'b'), (Key{0, 1}));
}

// Step 4 for a failed re-arm: the same as Close, and run() goes on. The log
// shows a told ("!a") before b is called; what a is told is checked below.
TEST_F(LoopTest, RunClosesTheSlotWhenTheRearmFails) {
    Client a;
    Client b;
    add(a, 'a');
    add(b, 'b');
    fake::kernel().write(a.handle, 1);
    fake::kernel().write(b.handle, 1);
    fake::kernel().fail_next_wait_async(Status::NoResource);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "a!ab");
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, a.handle);
    EXPECT_EQ(fake::kernel().armed_on(b.handle), 1U);
    Client c;
    EXPECT_EQ(add(c, 'c'), (Key{0, 1}));
}

// Step 4 for a failed re-arm, once more: the Waiter is told, with the status
// arm() answered. NoMemory is the realistic cause (the kernel allocates an
// observer on every arm), BadHandle the one a Waiter that closed its own
// handle and answered Rearm earns. The helper has the checks.
TEST_F(LoopTest, RunTellsTheWaiterWhenTheRearmFailsForWantOfMemory) {
    check_failed_rearm_is_told(Status::NoMemory);
}

TEST_F(LoopTest, RunTellsTheWaiterWhenTheRearmFailsOnABadHandle) {
    check_failed_rearm_is_told(Status::BadHandle);
}

// Only a failed re-arm is told. A Waiter that answers Close has released what
// it held, and the Loop makes no re-arm for it: the failure the fake has
// waiting is not met, and nothing is told.
TEST_F(LoopTest, RunDoesNotTellAWaiterThatAnsweredClose) {
    Client a;
    a.answer = Answer::Close;
    add(a, 'a');
    fake::kernel().write(a.handle, 1);
    fake::kernel().fail_next_wait_async(Status::NoMemory);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "a");
    EXPECT_TRUE(a.rearm_failures.empty());
    EXPECT_EQ(fake::kernel().waits().size(), 1U);
    EXPECT_EQ(fake::kernel().closes().size(), 1U);
}

// A Waiter that is told the slot is free may add(): the slot is its own to
// take again, even in a table that is otherwise full, in a new life. A packet
// of the earlier life, queued behind, does not reach what took the slot.
TEST_F(LoopTest, RunLetsTheWaiterAddFromOnRearmFailed) {
    Client a;
    Client replacement;
    std::array<Client, SLOTS - 1> others;
    auto const key = add(a, 'a');
    for (auto &other: others) {
        add(other, 'x');
    }
    auto replacement_key = Key{0xFFFF'FFFF, 0xFFFF'FFFF};
    a.during_rearm_failed = [&] {
        replacement_key = add(replacement, 'n');
        fake::kernel().write(replacement.handle, 1);
    };
    fake::kernel().write(a.handle, 1);
    fake::kernel().queue(m_port, signal_packet(key));
    fake::kernel().fail_next_wait_async(Status::NoMemory);

    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(replacement_key, (Key{0, 1}));
    EXPECT_EQ(m_log, "a!an");
    EXPECT_EQ(a.observed.size(), 1U);
    EXPECT_EQ(replacement.observed.size(), 1U);
    EXPECT_TRUE(replacement.rearm_failures.empty());
    EXPECT_EQ(fake::kernel().queued_on(m_port), 0U);
}

// Step 1: a packet that names no slot in use in its current life calls
// no Waiter, arms nothing and closes nothing, and run() goes on to the next.
TEST_F(LoopTest, RunDropsPacketsThatNameNoSlotInUse) {
    // Slot 0 lives once and is freed: generation 1 from here on.
    Client gone;
    gone.answer = Answer::Close;
    add(gone, 'g');
    fake::kernel().write(gone.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);

    Client a;
    Client b;
    ASSERT_EQ(add(a, 'a'), (Key{0, 1}));
    ASSERT_EQ(add(b, 'b'), (Key{1, 0}));
    m_log.clear();
    auto const waits_before = fake::kernel().waits().size();
    auto const closes_before = fake::kernel().closes().size();

    for (auto const &forged: {
             signal_packet(Key{0, 0}),           // slot 0's earlier life
             signal_packet(Key{0, 2}),           // a life slot 0 has not had
             signal_packet(Key{2, 0}),           // a free slot
             signal_packet(Key{SLOTS, 0}),       // past the table
             signal_packet(Key{0xFFFF'FFFF, 1}), // far past it
             user_packet(Key{1, 0}),             // b's key, but no wait's packet
         }) {
        fake::kernel().queue(m_port, forged);
    }
    // Behind them all, a packet that is b's.
    fake::kernel().write(b.handle, 1);

    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "b");
    EXPECT_EQ(fake::kernel().waits().size(), waits_before + 1);
    EXPECT_EQ(fake::kernel().closes().size(), closes_before);
    EXPECT_EQ(fake::kernel().queued_on(m_port), 0U);
}

// Step 1's bound: an index equal to the table's size is past it, whatever the
// memory there holds. In the test above, the bytes past the fixture's table
// happen not to look like a slot in use at generation 0, so a run() that let
// index == size through still dropped Key{SLOTS, 0} and passed. Here two
// Loops share one array, the first over its first slot and the second over
// the next, so that the slot just past the first Loop's table is in use at
// generation 0: such a run() reads it and calls the second Loop's Waiter.
TEST(LoopRun, DropsAnIndexOnePastTheTableWhateverLiesThere) {
    fake::kernel().reset();
    std::array<loop::Slot, 2> storage{};
    auto const port = fake::kernel().make_port();
    auto const neighbour_port = fake::kernel().make_port();
    loop::Loop table{port, std::span{storage}.first(1)};
    loop::Loop neighbour{neighbour_port, std::span{storage}.last(1)};

    auto log = std::string{};
    auto client = Client{};
    client.name = 'n';
    client.handle = fake::kernel().make_object();
    client.log = &log;
    auto const added = neighbour.add(client.handle, MESSAGES, client);
    ASSERT_TRUE(added.is_ok());
    ASSERT_EQ(added.unwrap(), (Key{0, 0}));

    fake::kernel().queue(port, signal_packet(Key{1, 0}));
    EXPECT_EQ(table.run(0), Status::TimeOut);
    EXPECT_EQ(log, "");
    EXPECT_EQ(fake::kernel().waits().size(), 1U);
    EXPECT_TRUE(fake::kernel().closes().empty());
}

// Step 1's "in use" is the handle, not the Waiter. The free slot of the test
// above was never used, so its Waiter is null and a run() that skipped the
// check crashed rather than failed. A slot freed by Close keeps its old Waiter
// unless run() clears it, and its generation is already that of its next
// life, so a packet with that key passes every other check: only the Invalid
// handle says the slot is free.
TEST_F(LoopTest, RunDropsAPacketForASlotFreedInItsNextLife) {
    Client gone;
    gone.answer = Answer::Close;
    add(gone, 'g');
    fake::kernel().write(gone.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    ASSERT_EQ(m_log, "g");

    fake::kernel().queue(m_port, signal_packet(Key{0, 1}));
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "g");
    EXPECT_EQ(fake::kernel().closes().size(), 1U);
    EXPECT_EQ(fake::kernel().waits().size(), 1U);
}

// The slot whose Waiter runs stays in use meanwhile: an add() from inside
// on_signals() takes another slot, whose wait run() then dispatches too.
TEST_F(LoopTest, RunKeepsTheSlotInUseWhileItsWaiterAdds) {
    Client a;
    Client added;
    auto added_key = Key{0xFFFF'FFFF, 0xFFFF'FFFF};
    add(a, 'a');
    auto added_once = false;
    a.during = [&] {
        // Only the first call adds. A flag rather than clearing a.during,
        // which would destroy this lambda while it runs.
        if (added_once) {
            return;
        }
        added_once = true;
        added_key = add(added, 'n');
        fake::kernel().write(added.handle, 1);
    };
    fake::kernel().write(a.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(added_key, (Key{1, 0}));
    EXPECT_EQ(m_log, "an");
}

// Each Waiter is called for its own slot only, round after round: a message
// for one calls the other not at all.
TEST_F(LoopTest, RunCallsEachWaiterForItsOwnSlotOnly) {
    Client a;
    Client b;
    add(a, 'a');
    add(b, 'b');
    fake::kernel().write(a.handle, 1);
    fake::kernel().write(b.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "ab");
    EXPECT_EQ(a.observed.size(), 1U);
    EXPECT_EQ(b.observed.size(), 1U);

    fake::kernel().write(b.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "abb");
    EXPECT_EQ(a.observed.size(), 1U);
    EXPECT_EQ(b.observed.size(), 2U);

    fake::kernel().write(a.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "abba");
    EXPECT_EQ(a.observed.size(), 2U);
    EXPECT_EQ(b.observed.size(), 2U);
}

// A slot is closed for its own Waiter's answer only: b answers Close and its
// slot is freed, and a, which answers Rearm in the same run, keeps its handle
// and its wait.
TEST_F(LoopTest, RunClosesOnlyTheSlotWhoseWaiterAnswersClose) {
    Client a;
    Client b;
    add(a, 'a');
    add(b, 'b');
    b.answer = Answer::Close;
    fake::kernel().write(a.handle, 1);
    fake::kernel().write(b.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "ab");
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, b.handle);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 1U);
    EXPECT_TRUE(fake::kernel().is_open(a.handle));
}
// --- Suspend and resume: a slot in use with nothing armed. ---

// Step 3 for Suspend: nothing is armed, closed or queued; the slot stays in
// use with its handle open. What the object asserts meanwhile stays in the
// object: the Waiter is not called for a message that arrives, and nothing
// is queued on the port for it.
TEST_F(LoopTest, RunLeavesASuspendedSlotInUseWithNothingArmed) {
    Client a;
    add_suspended(a, 'a');
    EXPECT_EQ(m_log, "a");

    fake::kernel().write(a.handle, 1);
    EXPECT_EQ(fake::kernel().queued_on(m_port), 0U);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "a");
    EXPECT_EQ(a.observed.size(), 1U);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 0U);
    EXPECT_TRUE(fake::kernel().is_open(a.handle));
}

// The key is still the slot's: Suspend bumps no generation, so the key add()
// handed out names the suspended slot, and resume() takes it.
TEST_F(LoopTest, RunKeepsTheKeyOfASuspendedSlotValid) {
    Client a;
    auto const key = add_suspended(a, 'a');
    EXPECT_EQ(key, (Key{0, 0}));
    EXPECT_EQ(m_loop.resume(key), Status::Ok);
}

// resume() arms the same wait: the same handle, port, key and signals, once.
// Nothing is called until the wait fires.
TEST_F(LoopTest, ResumeArmsTheWaitUnderTheSameKeyAndSignals) {
    Client a;
    auto const key = add_suspended(a, 'a');
    auto const waits_before = fake::kernel().waits().size();

    EXPECT_EQ(m_loop.resume(key), Status::Ok);
    auto const &waits = fake::kernel().waits();
    ASSERT_EQ(waits.size(), waits_before + 1);
    EXPECT_EQ(waits.back().handle, a.handle);
    EXPECT_EQ(waits.back().port, m_port);
    EXPECT_EQ(waits.back().key, key.raw());
    EXPECT_EQ(waits.back().signals, MESSAGES);
    EXPECT_EQ(waits.back().status, Status::Ok);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 1U);
    EXPECT_EQ(fake::kernel().queued_on(m_port), 0U);
    EXPECT_EQ(m_log, "a");
}

// The message that arrived while the slot was suspended is delivered once it
// is resumed, behind the packet that was already queued (I4): the wait is
// level-triggered, so it queues its packet when it is armed.
TEST_F(LoopTest, ResumeDeliversTheWaitingMessageBehindThePacketsAlreadyQueued) {
    Client a;
    Client b;
    auto const key = add_suspended(a, 'a');
    add(b, 'b');
    fake::kernel().write(a.handle, 1);
    fake::kernel().write(b.handle, 1);
    a.answer = Answer::Serve;

    EXPECT_EQ(m_loop.resume(key), Status::Ok);
    EXPECT_EQ(fake::kernel().queued_on(m_port), 2U);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "aba");
    EXPECT_EQ(a.observed.back(), Signals::Readable | Signals::Writable);
    // a read it and was armed again, as any Waiter that answers Rearm.
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 1U);
}

// The use the design has in mind: another slot's Waiter, whose work is what
// the suspended connection waited for, resumes it from its own on_signals().
TEST_F(LoopTest, ResumeFromAnotherWaitersOnSignalsArmsTheSlot) {
    Client a;
    Client b;
    auto const key = add_suspended(a, 'a');
    add(b, 'b');
    fake::kernel().write(a.handle, 1);
    a.answer = Answer::Serve;
    auto resumed = Status::BadHandle;
    b.during = [&] { resumed = m_loop.resume(key); };
    fake::kernel().write(b.handle, 1);

    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(resumed, Status::Ok);
    EXPECT_EQ(m_log, "aba");
}

// A peer that went while the slot was suspended is not seen until it is
// resumed, and is then seen as PeerClosed alone: the message was read before
// the Suspend. A Waiter that serves answers Close for it, as for any closed
// peer, and the Loop closes the handle and frees the slot.
TEST_F(LoopTest, ResumeShowsAPeerThatClosedWhileSuspendedAsPeerClosedAlone) {
    Client a;
    auto const key = add_suspended(a, 'a');
    fake::kernel().close_peer(a.handle);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "a");
    a.answer = Answer::Serve;

    EXPECT_EQ(m_loop.resume(key), Status::Ok);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "aa");
    EXPECT_EQ(a.observed.back(), Signals::PeerClosed);
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, a.handle);
    Client b;
    EXPECT_EQ(add(b, 'b'), (Key{0, 1}));
}

// resume() from inside the on_signals() that is about to answer Suspend: the
// slot is in use and in the key's life, but the Loop marks it suspended only
// once on_signals() has returned, so it answers BadState and changes nothing.
// The Suspend then goes through, and the slot is resumable.
TEST_F(LoopTest, ResumeInsideTheWaiterThatSuspendsAnswersBadState) {
    Client a;
    a.answer = Answer::Suspend;
    auto const key = add(a, 'a');
    auto inside = Status::Ok;
    auto waits_inside = usize{0};
    a.during = [&] {
        inside = m_loop.resume(key);
        waits_inside = fake::kernel().waits().size();
    };
    fake::kernel().write(a.handle, 1);

    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(inside, Status::BadState);
    // Only add()'s wait: the failed resume() armed nothing, and the Suspend
    // after it armed nothing either.
    EXPECT_EQ(waits_inside, 1U);
    EXPECT_EQ(fake::kernel().waits().size(), 1U);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 0U);
    EXPECT_EQ(m_log, "a");
    a.during = nullptr;
    a.answer = Answer::Serve;
    EXPECT_EQ(m_loop.resume(key), Status::Ok);
}

// An armed slot, whether its wait is waiting or its packet is queued, is not
// suspended: BadState, and no second wait.
TEST_F(LoopTest, ResumeAnswersBadStateForASlotThatIsArmed) {
    Client a;
    auto const key = add(a, 'a');
    EXPECT_EQ(m_loop.resume(key), Status::BadState);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 1U);
    EXPECT_EQ(fake::kernel().waits().size(), 1U);

    // Its packet queued, not yet taken.
    fake::kernel().write(a.handle, 1);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 0U);
    EXPECT_EQ(fake::kernel().queued_on(m_port), 1U);
    EXPECT_EQ(m_loop.resume(key), Status::BadState);
    EXPECT_EQ(fake::kernel().queued_on(m_port), 1U);
    EXPECT_EQ(fake::kernel().waits().size(), 1U);

    // The packet was taken once and the Waiter called once.
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "a");
    EXPECT_EQ(fake::kernel().waits().size(), 2U);
}

// A second resume() finds the slot armed: BadState, and the wait is armed
// once. Once the Waiter has answered Close and the slot is free, the same key
// is NotFound.
TEST_F(LoopTest, ResumeAnswersBadStateForASecondResumeThenNotFoundOnceClosed) {
    Client a;
    auto const key = add_suspended(a, 'a');
    auto const waits_before = fake::kernel().waits().size();

    EXPECT_EQ(m_loop.resume(key), Status::Ok);
    EXPECT_EQ(m_loop.resume(key), Status::BadState);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 1U);
    EXPECT_EQ(fake::kernel().waits().size(), waits_before + 1);

    a.answer = Answer::Close;
    fake::kernel().write(a.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "aa");
    EXPECT_EQ(m_loop.resume(key), Status::NotFound);
    EXPECT_EQ(fake::kernel().waits().size(), waits_before + 1);
}

// NotFound: the key names no slot in use in its life. Nothing changes, and a
// slot that is in use is not disturbed by a key that only looks like it.
TEST_F(LoopTest, ResumeAnswersNotFoundForAKeyThatNamesNoSlotInUse) {
    Client closed;
    Client live;
    closed.answer = Answer::Close;
    auto const closed_key = add(closed, 'c');
    add(live, 'l');
    fake::kernel().write(closed.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    ASSERT_EQ(m_log, "c");
    auto const waits_before = fake::kernel().waits().size();
    auto const closes_before = fake::kernel().closes().size();

    for (auto const stale: {
             closed_key,          // a slot closed since the key was handed out
             Key{0, 1},           // the same slot, free, in the life it has not had
             Key{2, 0},           // a slot never used
             Key{SLOTS, 0},       // the first index past the table
             Key{0xFFFF'FFFF, 0}, // far past it
         }) {
        EXPECT_EQ(m_loop.resume(stale), Status::NotFound);
    }
    EXPECT_EQ(fake::kernel().waits().size(), waits_before);
    EXPECT_EQ(fake::kernel().closes().size(), closes_before);
    EXPECT_EQ(fake::kernel().armed_on(live.handle), 1U);
    // The free slot is still free, in the life the failed resume()s left it.
    Client next;
    EXPECT_EQ(add(next, 'n'), (Key{0, 1}));
}

// NotFound for the old key of a reused slot, and the new life is left alone:
// it is suspended, nothing is armed for it, and its own key still resumes it.
TEST_F(LoopTest, ResumeAnswersNotFoundForTheOldKeyOfAReusedSlot) {
    Client first;
    auto const old_key = add_suspended(first, 'f');
    fake::kernel().close_peer(first.handle);
    first.answer = Answer::Serve;
    EXPECT_EQ(m_loop.resume(old_key), Status::Ok);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    ASSERT_EQ(m_log, "ff");

    Client second;
    auto const new_key = add_suspended(second, 's');
    ASSERT_EQ(old_key, (Key{0, 0}));
    ASSERT_EQ(new_key, (Key{0, 1}));
    auto const waits_before = fake::kernel().waits().size();

    EXPECT_EQ(m_loop.resume(old_key), Status::NotFound);
    EXPECT_EQ(fake::kernel().waits().size(), waits_before);
    EXPECT_EQ(fake::kernel().armed_on(second.handle), 0U);
    EXPECT_EQ(m_loop.resume(new_key), Status::Ok);
}

// A refused arm at resume() is a re-arm that failed: the handle is closed, the
// generation bumped, the slot freed, the Waiter told once with the cause --
// before resume() returns -- and resume() answers the cause.
TEST_F(LoopTest, ResumeFreesTheSlotAndTellsTheWaiterWhenTheArmIsRefused) {
    Client a;
    auto const key = add_suspended(a, 'a');
    auto const waits_before = fake::kernel().waits().size();
    fake::kernel().fail_next_wait_async(Status::NoMemory);

    EXPECT_EQ(m_loop.resume(key), Status::NoMemory);
    // Told already, before run() was called, and once.
    EXPECT_EQ(m_log, "a!a");
    EXPECT_EQ(a.rearm_failures, (std::vector<Status>{Status::NoMemory}));
    // The handle was closed, by the Loop, before the Waiter was told.
    EXPECT_FALSE(a.open_when_told);
    EXPECT_EQ(a.closes_when_told, 1U);
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, a.handle);
    EXPECT_EQ(closes[0].status, Status::Ok);
    // The refused wait is the only one made, and nothing is armed or queued.
    auto const &waits = fake::kernel().waits();
    ASSERT_EQ(waits.size(), waits_before + 1);
    EXPECT_EQ(waits.back().status, Status::NoMemory);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 0U);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "a!a");

    // The generation was bumped: the old key is stale, and the slot is free
    // in its next life.
    EXPECT_EQ(m_loop.resume(key), Status::NotFound);
    Client b;
    EXPECT_EQ(add(b, 'b'), (Key{0, 1}));
}

// The slot is free before the Waiter is told, so it may add() from
// on_rearm_failed(), the slot it was told about included, even in a table
// that is otherwise full.
TEST_F(LoopTest, ResumeLetsTheWaiterAddFromOnRearmFailed) {
    Client a;
    Client replacement;
    std::array<Client, SLOTS - 1> others;
    auto const key = add_suspended(a, 'a');
    for (auto &other: others) {
        add(other, 'x');
    }
    auto replacement_key = Key{0xFFFF'FFFF, 0xFFFF'FFFF};
    a.during_rearm_failed = [&] { replacement_key = add(replacement, 'n'); };
    fake::kernel().fail_next_wait_async(Status::NoMemory);

    EXPECT_EQ(m_loop.resume(key), Status::NoMemory);
    EXPECT_EQ(replacement_key, (Key{0, 1}));
    EXPECT_EQ(fake::kernel().armed_on(replacement.handle), 1U);
    EXPECT_EQ(m_loop.resume(key), Status::NotFound);
}

// A suspended slot is in use, so add() never takes it: with the table full,
// one of whose slots is suspended, add() answers NoResource and leaves the
// handle with the caller, and a slot freed later is taken before the
// suspended one, which keeps its life.
TEST_F(LoopTest, AddNeverTakesASuspendedSlot) {
    std::array<Client, SLOTS> clients;
    add(clients[0], 'x');
    add(clients[1], 'x');
    auto const key = add_suspended(clients[2], 's');
    add(clients[3], 'x');
    ASSERT_EQ(key, (Key{2, 0}));

    auto const spare = fake::kernel().make_object();
    Client latecomer;
    auto const full = m_loop.add(spare, MESSAGES, latecomer);
    ASSERT_TRUE(full.is_err());
    EXPECT_EQ(full.unwrap_err(), Status::NoResource);
    EXPECT_TRUE(fake::kernel().is_open(spare));

    // Slot 0 is freed; the lowest free slot is 0, not the suspended 2.
    clients[0].answer = Answer::Close;
    fake::kernel().write(clients[0].handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    Client next;
    EXPECT_EQ(add(next, 'n'), (Key{0, 1}));
    auto const full_again = m_loop.add(spare, MESSAGES, latecomer);
    ASSERT_TRUE(full_again.is_err());
    EXPECT_EQ(full_again.unwrap_err(), Status::NoResource);
    EXPECT_EQ(m_loop.resume(key), Status::Ok);
}

// The destructor closes a suspended slot's handle like any other, then the
// port, and calls and tells no Waiter.
TEST(Loop, ClosesTheHandleOfASuspendedSlotWhenItGoes) {
    fake::kernel().reset();
    auto const port = fake::kernel().make_port();
    auto const handle = fake::kernel().make_object();
    auto log = std::string{};
    Client suspended;
    suspended.name = 's';
    suspended.handle = handle;
    suspended.log = &log;
    suspended.answer = Answer::Suspend;
    {
        std::array<loop::Slot, 2> slots{};
        loop::Loop scoped{port, slots};
        ASSERT_TRUE(scoped.add(handle, MESSAGES, suspended).is_ok());
        fake::kernel().write(handle, 1);
        EXPECT_EQ(scoped.run(0), Status::TimeOut);
        EXPECT_TRUE(fake::kernel().closes().empty());
        EXPECT_TRUE(fake::kernel().is_open(handle));
    }
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 2U);
    EXPECT_EQ(closes[0].handle, handle);
    EXPECT_EQ(closes[0].status, Status::Ok);
    EXPECT_EQ(closes[1].handle, port);
    EXPECT_EQ(log, "s");
    EXPECT_TRUE(suspended.rearm_failures.empty());
}

// Step 1: a packet that names a suspended slot is dropped. No wait of the
// Loop's is armed for one, so only a forged packet can; it calls no Waiter,
// arms nothing and closes nothing, and the slot is as it was.
TEST_F(LoopTest, RunDropsAPacketThatNamesASuspendedSlot) {
    Client a;
    Client b;
    auto const key = add_suspended(a, 'a');
    add(b, 'b');
    auto const waits_before = fake::kernel().waits().size();
    auto const closes_before = fake::kernel().closes().size();
    fake::kernel().queue(m_port, signal_packet(key));
    // Behind it, a packet that is b's.
    fake::kernel().write(b.handle, 1);

    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "ab");
    EXPECT_EQ(a.observed.size(), 1U);
    // b's re-arm is the only wait made.
    EXPECT_EQ(fake::kernel().waits().size(), waits_before + 1);
    EXPECT_EQ(fake::kernel().closes().size(), closes_before);
    EXPECT_EQ(fake::kernel().queued_on(m_port), 0U);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 0U);
    // Still suspended, and resumable once.
    EXPECT_EQ(m_loop.resume(key), Status::Ok);
    EXPECT_EQ(m_loop.resume(key), Status::BadState);
}

// Waiters that answer only Rearm and Close are served as they always were,
// beside a slot that is suspended: the Rearm is armed again, the Close closes
// and frees its slot, and the suspended slot is neither called nor closed.
TEST_F(LoopTest, RunServesRearmAndCloseWaitersAsBeforeBesideASuspendedSlot) {
    Client suspended;
    Client rearmed;
    Client closed;
    suspended.answer = Answer::Suspend;
    closed.answer = Answer::Close;
    auto const suspended_key = add(suspended, 's');
    add(rearmed, 'r');
    add(closed, 'c');
    fake::kernel().write(suspended.handle, 1);
    fake::kernel().write(rearmed.handle, 1);
    fake::kernel().write(closed.handle, 1);

    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "src");
    EXPECT_EQ(fake::kernel().armed_on(rearmed.handle), 1U);
    EXPECT_EQ(fake::kernel().armed_on(suspended.handle), 0U);
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, closed.handle);
    Client next;
    EXPECT_EQ(add(next, 'n'), (Key{2, 1}));

    // The rest of the table is served in turns while the slot stays
    // suspended.
    fake::kernel().write(rearmed.handle, 2);
    fake::kernel().write(suspended.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "srcrr");
    EXPECT_EQ(m_loop.resume(suspended_key), Status::Ok);
}
} // namespace
