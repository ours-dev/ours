/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The global stage around a whole `ktest` run. An environment is what every
// test shares but no fixture should have to rebuild: SetUp runs once before
// the run's first test and TearDown once after its last, however the run went.
// Kept free of kernel headers so the host tests exercise the same registry.
#ifndef KTEST_ENVIRONMENT_HPP
#define KTEST_ENVIRONMENT_HPP 1

namespace ktest {

/// One global stage. Registered before the run that uses it — in the kernel a
/// test file does it at namespace scope, whose initializer runs with the
/// kernel's other boot-time constructors, and stores one pointer. A fixture is
/// the per-test shape of the same idea; an environment is the per-run one.
class Environment {
  public:
    // Out of line (environment.cpp): a trivial default body never lands a
    // coverage counter in the header, and the stages that override nothing
    // would show this line forever unreached.
    virtual ~Environment();

    /// Runs once before the run's first test. The defaults are no-ops: an
    /// environment overrides only the stage it actually has.
    virtual auto SetUp() -> void;
    virtual auto TearDown() -> void;
};

/// How many environments one registry holds. Every table here is fixed-size;
/// a run that finds a dropped registration reports it rather than passing
/// without the stage the tests were written against.
inline constexpr usize MAX_ENVIRONMENTS = 8;

/// Registers |environment| for the next and every later run, in registration
/// order. False when the registry is full, which the runner reports as a
/// framework error instead of silently running without the environment.
[[nodiscard]]
auto add_environment(Environment &environment) -> bool;

namespace internal {

    /// The registry the runner walks and the host tests reset between cases.
    struct EnvironmentRegistry {
        Environment *entries[MAX_ENVIRONMENTS]{};
        usize count{};
        usize dropped{};
    };

    auto environments() -> EnvironmentRegistry &;

    /// One step of the environment walk: the framework calls it with the
    /// environment and the caller's context. True when the environment's work
    /// recorded no failure — the answer that stops the SetUp pass.
    using EnvironmentStep = auto (*)(Environment &environment, void *context) -> bool;

    /// One run's environments: the SetUps it ran and how far it got, so the
    /// TearDowns unwind exactly the prefix that was set up, in reverse. The
    /// runner holds one per run and calls set_up() before its tests and
    /// tear_down() after them; the host tests drive the same two calls and
    /// check the order.
    class EnvironmentRun {
      public:
        /// SetUp on each registered environment, in registration order,
        /// stopping at the first whose |step| answers false. True when every
        /// registered environment was set up.
        auto set_up(EnvironmentStep step, void *context) -> bool;

        /// TearDown in reverse over the environments whose SetUp ran,
        /// including the one that failed — which is what makes TearDown the
        /// place to undo a half-finished SetUp. What |step| answers is
        /// ignored: a failed TearDown has nothing left to undo.
        auto tear_down(EnvironmentStep step, void *context) -> void;

      private:
        usize m_attempted{};
    };

} // namespace internal

} // namespace ktest

#endif // #ifndef KTEST_ENVIRONMENT_HPP
