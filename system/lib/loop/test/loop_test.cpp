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

// A slot is {handle 4, signals 4, Waiter pointer 8, generation 4 and 4 of
// padding}: 24 bytes on a target with 8-byte pointers, which every target of
// the project has. The function and the context it held before the Waiter,
// two pointers, made it 32. The root's table is LOOP_SLOTS of them
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
} // namespace
