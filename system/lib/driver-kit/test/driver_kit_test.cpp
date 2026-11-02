/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// driver_kit::Resources on the fake kernel: the ladder. A fake getter hands
// out real fake-kernel handles (so every handle_close is recorded and can be
// sequenced) and fails on the take a test names. What the cases pin: the
// destructor closing newest first, a mid-ladder failure leaving the prefix
// to the destructor, drop() releasing one root early without disturbing the
// rest, the table running out, and startup_handle's refusal line.
//
// ort::take_startup_handle is the user runtime's, defined in start.cpp,
// which does not build on the host: the test defines the symbol, answering
// Invalid, which is the one behavior startup_handle's guard adds value to.
#include <driver_kit/driver_kit.hpp>

#include <algorithm>
#include <array>
#include <ostream>
#include <vector>

#include "fake_kernel.hpp"
#include <gtest/gtest.h>

// The host stand-in for the user runtime's startup-handle table: always
// empty, which is the case startup_handle guards.
namespace ort {
auto take_startup_handle(ours::HandleType, u16) -> ours::Uandle {
    return ours::Uandle::Invalid;
}
} // namespace ort

// How gtest prints a status in a failure: by name, found by argument-
// dependent lookup in its own namespace. gtest looks for exactly the name
// PrintTo, hence the NOLINT.
namespace ours {
// NOLINTNEXTLINE(readability-identifier-naming): gtest's customisation point.
auto PrintTo(Status const status, std::ostream *const out) -> void {
    *out << to_string(status);
}
} // namespace ours

namespace {
using ours::Status;
using ours::Uandle;

/// The fake getter's state, reset by the fixture: it answers every take with
/// a fresh open handle except the take whose number FAIL_AT names, which it
/// refuses with NoMemory. Getter is a plain function pointer, so the state
/// lives here.
usize CALLS = 0;
usize FAIL_AT = 0;

auto fake_get(ours_resource::Protocol const &) -> ustl::Result<Uandle, Status> {
    CALLS += 1;
    if (FAIL_AT != 0 && CALLS == FAIL_AT) {
        return ustl::err(Status::NoMemory);
    }
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    if (auto const created = fake::kernel().channel_create(&client, &server);
        created != Status::Ok) {
        return ustl::err(created);
    }
    // The client end is a handle like any other for the ladder's purposes;
    // the server end stays open so the client's close is a plain close.
    return ustl::ok(client);
}

class DriverKitTest: public testing::Test {
  protected:
    DriverKitTest() {
        fake::kernel().reset();
        CALLS = 0;
        FAIL_AT = 0;
    }

    /// The handles close() recorded since |marker|, oldest first.
    static auto closed_since(usize const marker) -> std::vector<Uandle> {
        auto const &all = fake::kernel().closed();
        return {all.begin() + static_cast<std::ptrdiff_t>(marker), all.end()};
    }

    static auto marker() -> usize {
        return fake::kernel().closed().size();
    }
};

TEST_F(DriverKitTest, TheDestructorClosesWhatItHoldsNewestFirst) {
    auto const before = marker();
    auto held = std::vector<Uandle>{};
    {
        auto resources = driver_kit::Resources(&fake_get);
        for (auto const kind: {driver_kit::Kind::Mmio, driver_kit::Kind::Ioport,
                               driver_kit::Kind::Iommu, driver_kit::Kind::Irq}) {
            auto const taken = resources.take(kind);
            ASSERT_TRUE(taken.is_ok());
            held.push_back(taken.unwrap());
            EXPECT_TRUE(fake::kernel().is_open(taken.unwrap()));
        }
        EXPECT_EQ(held.size(), usize{4});
        // Nothing is closed while the ladder is alive.
        EXPECT_TRUE(closed_since(before).empty());
    }
    // The destructor closed them all, newest first.
    auto const closed = closed_since(before);
    ASSERT_EQ(closed.size(), usize{4});
    EXPECT_EQ(closed[0], held[3]);
    EXPECT_EQ(closed[1], held[2]);
    EXPECT_EQ(closed[2], held[1]);
    EXPECT_EQ(closed[3], held[0]);
}

TEST_F(DriverKitTest, AMidLadderFailureLeavesThePrefixToTheDestructor) {
    auto const before = marker();
    FAIL_AT = 3;
    auto taken = std::vector<Uandle>{};
    {
        auto resources = driver_kit::Resources(&fake_get);
        auto const first = resources.take(driver_kit::Kind::Mmio);
        auto const second = resources.take(driver_kit::Kind::Ioport);
        auto const refused = resources.take(driver_kit::Kind::Iommu);
        ASSERT_TRUE(first.is_ok());
        ASSERT_TRUE(second.is_ok());
        ASSERT_TRUE(refused.is_err());
        EXPECT_EQ(refused.unwrap_err(), Status::NoMemory);
        taken.push_back(first.unwrap());
        taken.push_back(second.unwrap());
        // The two the ladder holds are still open, and nothing was closed
        // for the failure: the caller decides to return, and the destructor
        // does the closing.
        EXPECT_TRUE(fake::kernel().is_open(taken[0]));
        EXPECT_TRUE(fake::kernel().is_open(taken[1]));
        EXPECT_TRUE(closed_since(before).empty());
    }
    auto const closed = closed_since(before);
    ASSERT_EQ(closed.size(), usize{2});
    EXPECT_EQ(closed[0], taken[1]);
    EXPECT_EQ(closed[1], taken[0]);
}

TEST_F(DriverKitTest, DropReleasesOneRootEarlyWithoutDisturbingTheRest) {
    auto const before = marker();
    auto held = std::vector<Uandle>{};
    {
        auto resources = driver_kit::Resources(&fake_get);
        for (auto const kind:
             {driver_kit::Kind::Mmio, driver_kit::Kind::Ioport, driver_kit::Kind::Irq}) {
            held.push_back(resources.take(kind).unwrap());
        }
        auto const after_takes = marker();

        // The middle root, done being asked: dropped, closed, and out of the
        // ladder's book.
        resources.drop(held[1]);
        auto const dropped = closed_since(after_takes);
        ASSERT_EQ(dropped.size(), usize{1});
        EXPECT_EQ(dropped[0], held[1]);
        EXPECT_FALSE(fake::kernel().is_open(held[1]));
        EXPECT_TRUE(fake::kernel().is_open(held[0]));
        EXPECT_TRUE(fake::kernel().is_open(held[2]));
    }
    // The destructor closes the rest, newest first; the dropped one is not
    // closed twice.
    auto const rest = closed_since(before);
    ASSERT_EQ(rest.size(), usize{3});
    EXPECT_EQ(rest[0], held[1]);
    EXPECT_EQ(rest[1], held[2]);
    EXPECT_EQ(rest[2], held[0]);
}

TEST_F(DriverKitTest, TheLadderRunsOut) {
    auto resources = driver_kit::Resources(&fake_get);
    auto const before = marker();
    for (auto index = usize{0}; index < 5; ++index) {
        auto const kind = index == 4 ? driver_kit::Kind::Irq : driver_kit::Kind::Mmio;
        ASSERT_TRUE(resources.take(kind).is_ok());
    }
    auto const refused = resources.take(driver_kit::Kind::Ioport);
    ASSERT_TRUE(refused.is_err());
    EXPECT_EQ(refused.unwrap_err(), Status::NoResource);
    // The refusal held nothing and closed nothing.
    EXPECT_TRUE(closed_since(before).empty());
}

TEST_F(DriverKitTest, AStartupHandleMissingSaysSoAndErrors) {
    // The host stand-in above answers Invalid, which is the guard's case:
    // the line on the console, and the status to return.
    auto const missing = driver_kit::startup_handle(ours::HandleType::VmaRoot, "VmaRoot");
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(missing.unwrap_err(), Status::NotFound);
    auto const &lines = fake::kernel().debug_writes();
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.back(), "driver-kit: started without VmaRoot\n");
}

} // namespace
