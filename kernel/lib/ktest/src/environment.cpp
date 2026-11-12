/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The environment registry. One fixed table, filled by boot-time initializers
// and walked once per `ktest` run; no heap and no constructor order to depend
// on, because every registration appends into its own slot.
#include <ktest/environment.hpp>

namespace ktest {

Environment::~Environment() = default;

auto Environment::SetUp() -> void {}

auto Environment::TearDown() -> void {}

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

    auto EnvironmentRun::set_up(EnvironmentStep step, void *const context) -> bool {
        auto &registry = environments();
        m_attempted = 0;
        while (m_attempted < registry.count) {
            auto const ok = step(*registry.entries[m_attempted], context);
            m_attempted += 1;
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    auto EnvironmentRun::tear_down(EnvironmentStep step, void *const context) -> void {
        auto &registry = environments();
        while (m_attempted > 0) {
            m_attempted -= 1;
            step(*registry.entries[m_attempted], context);
        }
    }

} // namespace internal

} // namespace ktest
