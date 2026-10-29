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

/// What a client's callback answers.
enum class Answer : u8 {
    /// Read one message and answer Rearm; once nothing is left to read and
    /// the peer is gone, answer Close. What a connection's callback does.
    Serve,

    /// Answer Close at once.
    Close,
};

/// One slot's context: what its callback does and what it saw.
struct Client {
    char name = '?';
    Uandle handle = Uandle::Invalid;
    Answer answer = Answer::Serve;

    /// Run inside the callback before it answers, when set.
    std::function<void()> during;

    /// Where every callback appends its client's name, in call order.
    std::string *log = nullptr;

    /// The observed signals of each call.
    std::vector<Signals> observed;
};

auto serve(void *const context, Signals const observed) -> Action {
    auto &client = *static_cast<Client *>(context);
    client.log->push_back(client.name);
    client.observed.push_back(observed);
    if (client.during) {
        client.during();
    }
    if (client.answer == Answer::Close) {
        return Action::Close;
    }
    if ((observed & Signals::Readable) != Signals::None) {
        // One message per callback (I4), whatever else is queued.
        auto _ = fake::kernel().read(client.handle);
        return Action::Rearm;
    }
    return Action::Close;
}

/// A Client that waits as a Waiter: the slot's context is this object, and
/// on_signals() does what serve() does for the Client inside it, so that a
/// function registration and a Waiter one behave alike and a test tells them
/// apart only by how they were added.
struct WaiterClient final: loop::Waiter {
    Client client;

    auto on_signals(Signals const observed) -> Action override {
        return serve(&client, observed);
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
        auto added = m_loop.add(client.handle, signals, serve, &client);
        EXPECT_TRUE(added.is_ok());
        if (added.is_err()) {
            return Key{0xFFFF'FFFF, 0xFFFF'FFFF};
        }
        return added.unwrap();
    }

    /// As add(), but |waiter| is added as a Waiter.
    auto add_waiter(WaiterClient &waiter, char const name, Signals const signals = MESSAGES)
        -> Key {
        waiter.client.name = name;
        waiter.client.handle = fake::kernel().make_object();
        waiter.client.log = &m_log;
        auto added = m_loop.add(waiter.client.handle, signals, waiter);
        EXPECT_TRUE(added.is_ok());
        if (added.is_err()) {
            return Key{0xFFFF'FFFF, 0xFFFF'FFFF};
        }
        return added.unwrap();
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
}

TEST_F(LoopTest, AddRefusesAnInvalidHandleNoSignalsOrNoCallback) {
    auto const handle = fake::kernel().make_object();
    Client client;
    client.log = &m_log;
    auto const no_handle = m_loop.add(Uandle::Invalid, MESSAGES, serve, &client);
    ASSERT_TRUE(no_handle.is_err());
    EXPECT_EQ(no_handle.unwrap_err(), Status::InvArgs);
    auto const no_signals = m_loop.add(handle, Signals::None, serve, &client);
    ASSERT_TRUE(no_signals.is_err());
    EXPECT_EQ(no_signals.unwrap_err(), Status::InvArgs);
    auto const no_callback = m_loop.add(handle, MESSAGES, nullptr, &client);
    ASSERT_TRUE(no_callback.is_err());
    EXPECT_EQ(no_callback.unwrap_err(), Status::InvArgs);
    EXPECT_TRUE(fake::kernel().waits().empty());
    EXPECT_TRUE(fake::kernel().is_open(handle));
}

TEST_F(LoopTest, AddAnswersNoResourceWhenEverySlotIsInUse) {
    std::array<Client, SLOTS> clients;
    for (auto &client: clients) {
        add(client, 'x');
    }
    auto const spare = fake::kernel().make_object();
    Client client;
    auto const full = m_loop.add(spare, MESSAGES, serve, &client);
    ASSERT_TRUE(full.is_err());
    EXPECT_EQ(full.unwrap_err(), Status::NoResource);
    EXPECT_TRUE(fake::kernel().is_open(spare));
}

TEST_F(LoopTest, AddLeavesTheSlotFreeAndTheHandleWithTheCallerWhenTheWaitFails) {
    auto const handle = fake::kernel().make_object();
    Client client;
    fake::kernel().fail_next_wait_async(Status::AccessDenied);
    auto const refused = m_loop.add(handle, MESSAGES, serve, &client);
    ASSERT_TRUE(refused.is_err());
    EXPECT_EQ(refused.unwrap_err(), Status::AccessDenied);
    EXPECT_TRUE(fake::kernel().is_open(handle));
    EXPECT_TRUE(fake::kernel().closes().empty());
    // No key was handed out, so the slot is taken again in the same life.
    Client a;
    EXPECT_EQ(add(a, 'a'), (Key{0, 0}));
}

// --- add() with a Waiter: the same slot, key and wait as the other add(). ---

TEST_F(LoopTest, AddWithAWaiterTakesTheLowestFreeSlotAndArmsOneWaitUnderItsKey) {
    Client a;
    WaiterClient w;
    add(a, 'a');
    auto const key = add_waiter(w, 'w');
    EXPECT_EQ(key, (Key{1, 0}));
    auto const &waits = fake::kernel().waits();
    ASSERT_EQ(waits.size(), 2U);
    EXPECT_EQ(waits[1].handle, w.client.handle);
    EXPECT_EQ(waits[1].port, m_port);
    EXPECT_EQ(waits[1].key, key.raw());
    EXPECT_EQ(waits[1].signals, MESSAGES);
    EXPECT_EQ(waits[1].status, Status::Ok);
    EXPECT_EQ(fake::kernel().armed_on(w.client.handle), 1U);
    // Adding is not calling.
    EXPECT_TRUE(m_log.empty());
}

TEST_F(LoopTest, AddWithAWaiterRefusesAnInvalidHandleOrNoSignals) {
    auto const handle = fake::kernel().make_object();
    WaiterClient w;
    w.client.log = &m_log;
    auto const no_handle = m_loop.add(Uandle::Invalid, MESSAGES, w);
    ASSERT_TRUE(no_handle.is_err());
    EXPECT_EQ(no_handle.unwrap_err(), Status::InvArgs);
    auto const no_signals = m_loop.add(handle, Signals::None, w);
    ASSERT_TRUE(no_signals.is_err());
    EXPECT_EQ(no_signals.unwrap_err(), Status::InvArgs);
    EXPECT_TRUE(fake::kernel().waits().empty());
    EXPECT_TRUE(fake::kernel().is_open(handle));
    // Nothing was taken: the first slot is still the lowest free one.
    WaiterClient first;
    EXPECT_EQ(add_waiter(first, 'f'), (Key{0, 0}));
}

TEST_F(LoopTest, AddWithAWaiterAnswersNoResourceWhenEverySlotIsInUse) {
    // The table is shared between the two forms: odd slots hold functions,
    // even ones Waiters, and a Waiter finds no slot of either kind free.
    std::array<Client, SLOTS> clients;
    std::array<WaiterClient, SLOTS> waiters;
    for (auto index = usize{0}; index < SLOTS; ++index) {
        if (index % 2 == 0) {
            add_waiter(waiters[index], 'w');
        } else {
            add(clients[index], 'c');
        }
    }
    auto const spare = fake::kernel().make_object();
    WaiterClient w;
    auto const full = m_loop.add(spare, MESSAGES, w);
    ASSERT_TRUE(full.is_err());
    EXPECT_EQ(full.unwrap_err(), Status::NoResource);
    EXPECT_TRUE(fake::kernel().is_open(spare));
    EXPECT_EQ(fake::kernel().waits().size(), SLOTS);
}

TEST_F(LoopTest, AddWithAWaiterLeavesTheSlotFreeAndTheHandleWithTheCallerWhenTheWaitFails) {
    auto const handle = fake::kernel().make_object();
    WaiterClient w;
    fake::kernel().fail_next_wait_async(Status::AccessDenied);
    auto const refused = m_loop.add(handle, MESSAGES, w);
    ASSERT_TRUE(refused.is_err());
    EXPECT_EQ(refused.unwrap_err(), Status::AccessDenied);
    EXPECT_TRUE(fake::kernel().is_open(handle));
    EXPECT_TRUE(fake::kernel().closes().empty());
    // No key was handed out, so the slot is taken again in the same life.
    WaiterClient again;
    EXPECT_EQ(add_waiter(again, 'w'), (Key{0, 0}));
}

TEST(Loop, ClosesItsHandlesAndThenItsPortWhenItGoes) {
    fake::kernel().reset();
    auto const port = fake::kernel().make_port();
    auto const first = fake::kernel().make_object();
    auto const second = fake::kernel().make_object();
    {
        std::array<loop::Slot, 2> slots{};
        loop::Loop scoped{port, slots};
        auto const one = scoped.add(first, MESSAGES, serve, nullptr);
        auto const two = scoped.add(second, MESSAGES, serve, nullptr);
        ASSERT_TRUE(one.is_ok() && two.is_ok());
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

// Step 2: the callback of the slot whose wait fired, with its context and the
// packet's observed signals; no other callback.
TEST_F(LoopTest, RunCallsTheCallbackOfTheSlotWhoseWaitFired) {
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

// I4: one callback per packet, then re-arm. a has three messages and b one;
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
// the slot is free in a new life.
TEST_F(LoopTest, RunClosesTheSlotWhenItsCallbackAnswersClose) {
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
    Client b;
    EXPECT_EQ(add(b, 'b'), (Key{0, 1}));
}

// Step 4 for a failed re-arm: the same as Close, and run() goes on.
TEST_F(LoopTest, RunClosesTheSlotWhenTheRearmFails) {
    Client a;
    Client b;
    add(a, 'a');
    add(b, 'b');
    fake::kernel().write(a.handle, 1);
    fake::kernel().write(b.handle, 1);
    fake::kernel().fail_next_wait_async(Status::NoResource);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "ab");
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, a.handle);
    EXPECT_EQ(fake::kernel().armed_on(b.handle), 1U);
    Client c;
    EXPECT_EQ(add(c, 'c'), (Key{0, 1}));
}

// Step 1: a packet that names no slot in use in its current life calls
// nothing, arms nothing and closes nothing, and run() goes on to the next.
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
// generation 0: such a run() reads it and calls the second Loop's callback.
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
    auto const added = neighbour.add(client.handle, MESSAGES, serve, &client);
    ASSERT_TRUE(added.is_ok());
    ASSERT_EQ(added.unwrap(), (Key{0, 0}));

    fake::kernel().queue(port, signal_packet(Key{1, 0}));
    EXPECT_EQ(table.run(0), Status::TimeOut);
    EXPECT_EQ(log, "");
    EXPECT_EQ(fake::kernel().waits().size(), 1U);
    EXPECT_TRUE(fake::kernel().closes().empty());
}

// Step 1's "in use" is the handle, not the callback. The free slot of the
// test above was never used, so its callback is null and a run() that skipped
// the check crashed rather than failed. A slot freed by Close keeps its old
// callback and context unless run() clears them, and its generation is
// already that of its next life, so a packet with that key passes every other
// check: only the Invalid handle says the slot is free.
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

// The slot whose callback runs stays in use meanwhile: an add() from inside
// the callback takes another slot, whose wait run() then dispatches too.
TEST_F(LoopTest, RunKeepsTheSlotInUseWhileItsCallbackAdds) {
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

// --- run() with a Waiter: on_signals() is what a slot's wait calls. ---

// Step 2: the Waiter whose slot's wait fired is called, once, with the
// packet's observed signals; no other Waiter.
TEST_F(LoopTest, RunCallsTheWaiterOfTheSlotWhoseWaitFiredWithTheObservedSignals) {
    WaiterClient a;
    WaiterClient b;
    add_waiter(a, 'a');
    add_waiter(b, 'b');
    fake::kernel().write(b.client.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "b");
    EXPECT_EQ(b.client.observed, (std::vector<Signals>{Signals::Readable | Signals::Writable}));
    EXPECT_TRUE(a.client.observed.empty());
}

// Step 2 once more, for the peer going away: what the Waiter is told is what
// the object asserted, whatever it is.
TEST_F(LoopTest, RunTellsAWaiterWhatTheObjectAsserted) {
    WaiterClient w;
    add_waiter(w, 'w');
    fake::kernel().close_peer(w.client.handle);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(w.client.observed, (std::vector<Signals>{Signals::PeerClosed}));
}

// Step 3: a Waiter's Rearm arms the same wait again -- same handle, key and
// signals -- and closes nothing.
TEST_F(LoopTest, RunRearmsAWaiterUnderTheSameKeyAndSignals) {
    WaiterClient w;
    auto const key = add_waiter(w, 'w');
    fake::kernel().write(w.client.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "w");
    auto const &waits = fake::kernel().waits();
    ASSERT_EQ(waits.size(), 2U);
    EXPECT_EQ(waits[1].handle, w.client.handle);
    EXPECT_EQ(waits[1].port, m_port);
    EXPECT_EQ(waits[1].key, key.raw());
    EXPECT_EQ(waits[1].signals, MESSAGES);
    EXPECT_EQ(fake::kernel().armed_on(w.client.handle), 1U);
    EXPECT_TRUE(fake::kernel().closes().empty());
    EXPECT_TRUE(fake::kernel().is_open(w.client.handle));
}

// Step 4 for a Waiter's Close: the handle is closed once, nothing is armed
// for it, the Waiter is not called again, and the slot is free in a new life.
TEST_F(LoopTest, RunClosesTheSlotWhenItsWaiterAnswersClose) {
    WaiterClient w;
    w.client.answer = Answer::Close;
    add_waiter(w, 'w');
    fake::kernel().write(w.client.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "w");
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, w.client.handle);
    EXPECT_EQ(closes[0].status, Status::Ok);
    EXPECT_EQ(fake::kernel().waits().size(), 1U);
    EXPECT_EQ(fake::kernel().armed_on(w.client.handle), 0U);
    WaiterClient next;
    EXPECT_EQ(add_waiter(next, 'n'), (Key{0, 1}));
}

// A Waiter and a function share one Loop, and each is called for its own
// slot only: a is a function and w a Waiter, and a message for one calls the
// other not at all.
TEST_F(LoopTest, RunCallsAWaiterAndAFunctionEachForItsOwnSlot) {
    Client a;
    WaiterClient w;
    add(a, 'a');
    add_waiter(w, 'w');
    fake::kernel().write(a.handle, 1);
    fake::kernel().write(w.client.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "aw");
    EXPECT_EQ(a.observed.size(), 1U);
    EXPECT_EQ(w.client.observed.size(), 1U);

    fake::kernel().write(w.client.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "aww");
    EXPECT_EQ(a.observed.size(), 1U);
    EXPECT_EQ(w.client.observed.size(), 2U);

    fake::kernel().write(a.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "awwa");
    EXPECT_EQ(a.observed.size(), 2U);
    EXPECT_EQ(w.client.observed.size(), 2U);
}

// Each form closes only its own: a Waiter that answers Close frees its slot
// and leaves a function's alone, and the other way round.
TEST_F(LoopTest, RunClosesOnlyTheSlotThatAnswersCloseWhateverFormTheOtherIs) {
    Client a;
    WaiterClient w;
    add(a, 'a');
    add_waiter(w, 'w');
    w.client.answer = Answer::Close;
    fake::kernel().write(a.handle, 1);
    fake::kernel().write(w.client.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(m_log, "aw");
    auto const &closes = fake::kernel().closes();
    ASSERT_EQ(closes.size(), 1U);
    EXPECT_EQ(closes[0].handle, w.client.handle);
    EXPECT_EQ(fake::kernel().armed_on(a.handle), 1U);
    EXPECT_TRUE(fake::kernel().is_open(a.handle));
}

// A Waiter may add() from inside on_signals(), a Waiter or a function: the
// slot that is running is in use, so the new one is another.
TEST_F(LoopTest, RunKeepsTheSlotInUseWhileAWaiterAdds) {
    WaiterClient w;
    WaiterClient added;
    auto added_key = Key{0xFFFF'FFFF, 0xFFFF'FFFF};
    add_waiter(w, 'w');
    auto added_once = false;
    w.client.during = [&] {
        if (added_once) {
            return;
        }
        added_once = true;
        added_key = add_waiter(added, 'n');
        fake::kernel().write(added.client.handle, 1);
    };
    fake::kernel().write(w.client.handle, 1);
    EXPECT_EQ(m_loop.run(0), Status::TimeOut);
    EXPECT_EQ(added_key, (Key{1, 0}));
    EXPECT_EQ(m_log, "wn");
}
} // namespace
