/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The environment registry and the stages' default framing. One fixed table,
// filled by boot-time initializers and walked once per `ktest` run; no heap
// and no constructor order to depend on, because every registration appends
// into its own slot. The walk frames each stage itself, through the shared
// stage runner, so no caller passes machinery in.
#include <ktest/environment.hpp>
#include <ktest/run_stage.hpp>

namespace ktest {

Environment::~Environment() = default;

auto Environment::SetUp() -> void {}

auto Environment::TearDown() -> void {}

auto Environment::set_up_stage(hsien::Reporter &reporter) -> bool {
    return internal::run_hook(
        "ktest.EnvironmentSetUp", [&](::hsien::Context &) { SetUp(); }, reporter);
}

auto Environment::tear_down_stage(hsien::Reporter &reporter) -> void {
    auto const ok = internal::run_hook(
        "ktest.EnvironmentTearDown", [&](::hsien::Context &) { TearDown(); }, reporter);
    if (!ok) {
        // The walk has nothing left to undo on a failed TearDown; this one
        // error line is what tells the console the stage did not come down
        // clean.
        internal::framework_error("the environment teardown failed");
    }
}

namespace {

    internal::EnvironmentRegistry REGISTRY;

} // namespace

auto add_environment(Environment &environment) -> bool {
    auto &registry = internal::environments();
    if (registry.count == MAX_ENVIRONMENTS) {
        registry.dropped += 1;
        return false;
    }
    registry.entries[registry.count] = &environment;
    registry.count += 1;
    return true;
}

namespace internal {

    auto environments() -> EnvironmentRegistry & {
        return REGISTRY;
    }

    auto EnvironmentRun::set_up(hsien::Reporter &reporter) -> bool {
        auto &registry = environments();
        m_attempted = 0;
        while (m_attempted < registry.count) {
            auto const ok = registry.entries[m_attempted]->set_up_stage(reporter);
            m_attempted += 1;
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    auto EnvironmentRun::tear_down(hsien::Reporter &reporter) -> void {
        auto &registry = environments();
        while (m_attempted > 0) {
            m_attempted -= 1;
            registry.entries[m_attempted]->tear_down_stage(reporter);
        }
    }

} // namespace internal

} // namespace ktest
