/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Tests that share state: a fixture the `KTEST_F` tests of one suite are
// written against, a hook that frames the whole suite once, and the pieces of
// both that the host tests exercise — the base classes, the lifecycle the
// adapter runs, and the plan that decides when the suite hooks fire.
//
// Everything here is free of kernel headers, so the host suite runs it
// against the real macro expansions; only the sections the records land in
// are the kernel's.
#ifndef KTEST_FIXTURE_HPP
#define KTEST_FIXTURE_HPP 1

#include <ktest/run_state.hpp>

#include <memory>
#include <source_location>
#include <string_view>
#include <utility>

#include <gktl/types.hpp>
#include <hsien/test.hpp>

/// Where a record lands. On the kernel's ELF images that is the section the
/// runner walks; the host has no such sections and its tests name the records
/// directly instead of enumerating them, so the attribute is dropped there.
#if defined(__ELF__)
#define KTEST_RECORD_SECTION(name) LINK_SECTION(name)
#else
#define KTEST_RECORD_SECTION(name)
#endif

namespace ktest {

/// The state a suite's tests share. `KTEST_F(Fixture, Name)` writes its body
/// as a member of a class derived from the fixture, so the body reads the
/// fixture's members through `this` and may shadow SetUp and TearDown to
/// build and undo that state around each test. The fixture must be default
/// constructible; its members the bodies touch are `protected:` or public,
/// exactly as in GoogleTest.
///
/// The base is CRTP, not polymorphic: the stages resolve statically to the
/// derived class's SetUp/TearDown, and the test's instance is one static
/// object per record, never deleted through the base — so there is no virtual
/// table and no virtual destructor to pay for. Shadowing one stage and not
/// the other is fine; shadowing neither keeps the no-op defaults.
template <typename Derived>
class Fixture {
  public:
    /// Runs before each test's body. A failure recorded here — an assertion
    /// or a KTEST_SKIP — leaves the body out: there is no point running a
    /// check against state that could not be built.
    auto SetUp() -> void {}

    /// Runs after each test's body, however far the test got: after a pass,
    /// after a failed check, after an ASSERT returned, and after a SetUp
    /// failure, which is what makes this the place to undo SetUp.
    auto TearDown() -> void {}

    /// The stage the generated test class's adapter calls before the body,
    /// resolved statically to the derived class's SetUp.
    auto run_set_up() -> void {
        static_cast<Derived *>(this)->SetUp();
    }

    /// The stage the generated test class's adapter calls after the body,
    /// resolved statically to the derived class's TearDown.
    auto run_tear_down() -> void {
        static_cast<Derived *>(this)->TearDown();
    }
};

namespace internal {

    /// Which end of a suite's tests a hook frames.
    enum class SuiteHookKind : u8 {
        /// Runs once before the suite's first test of the run.
        SetUp,
        /// Runs once after the suite's last test of the run.
        TearDown,
    };

    /// One suite hook record: what `KTEST_SUITE_SETUP(fixture)` and
    /// `KTEST_SUITE_TEARDOWN(fixture)` put in `.ktest.suite_hooks` for the
    /// runner to find. The suite is the fixture's name — the prefix of every
    /// test name the fixture's `KTEST_F`s carry.
    struct SuiteHook {
        char const *name;
        char const *suite;
        SuiteHookKind kind;
        void (*invoke)(::hsien::Context &);
    };

    /// A run's decisions about the suites that carry hooks: which hook runs
    /// before the first selected test of a suite and which after its last,
    /// and which suites leave their tests out because the setup failed. Built
    /// over the hook section, the records and the run's selection — the same
    /// predicate the run loop selects with, so the counts match what runs —
    /// then asked once per selected test. All of it is string logic over
    /// fixed tables, which is what makes it host-testable.
    class SuitePlan {
      public:
        /// How many suites carrying hooks one run tracks. Like the run
        /// options: a fixed table, and an authoring bug beyond it is reported,
        /// not silently truncated.
        static constexpr usize MAX_SUITES = 8;

        /// |selected| answers for a record and its section position whether
        /// this run runs it.
        template <typename Selected>
        SuitePlan(SuiteHook const *const hooks_begin, SuiteHook const *const hooks_end,
                  ::hsien::TestRecord const *const records_begin,
                  ::hsien::TestRecord const *const records_end, Selected &&selected) {
            for (auto index = usize{0}; records_begin + index != records_end; ++index) {
                auto const &record = records_begin[index];
                if (!selected(record, index)) {
                    continue;
                }
                auto const suite = suite_of(record.name);
                // A suite no hook names has nothing to plan.
                auto hooked = false;
                for (auto const *hook = hooks_begin; hook != hooks_end; ++hook) {
                    if (std::string_view{hook->suite} == suite) {
                        hooked = true;
                        break;
                    }
                }
                if (!hooked) {
                    continue;
                }
                auto *slot = find_slot(suite);
                if (slot == nullptr) {
                    if (m_count == MAX_SUITES) {
                        m_valid = false;
                        return;
                    }
                    slot = &m_slots[m_count];
                    m_count += 1;
                    *slot = Slot{.suite = suite};
                }
                for (auto const *hook = hooks_begin; hook != hooks_end; ++hook) {
                    if (std::string_view{hook->suite} != suite) {
                        continue;
                    }
                    auto *into = &slot->teardown;
                    if (hook->kind == SuiteHookKind::SetUp) {
                        into = &slot->setup;
                    }
                    if (*into == hook) {
                        // This suite's earlier record attached it already;
                        // the hooks are per suite, the count per test.
                        continue;
                    }
                    if (*into != nullptr) {
                        // The suite declares the same kind of hook twice;
                        // which one would run is nobody's guess.
                        m_valid = false;
                        return;
                    }
                    *into = hook;
                }
                slot->remaining += 1;
            }
        }

        /// False when a selected suite declares a hook twice, or more suites
        /// carry hooks than the plan tracks. The run reports it and stops
        /// before any test runs.
        [[nodiscard]]
        auto valid() const -> bool {
            return m_valid;
        }

        /// The SetUp hook to run before |name|'s test: its suite's first of
        /// the run, which is where "once" happens. Null for every later test
        /// of the suite, and for suites with only a teardown hook.
        auto before(std::string_view const name) -> SuiteHook const * {
            auto *const slot = find_slot(suite_of(name));
            if (slot == nullptr) {
                return nullptr;
            }
            if (!slot->setup_ran) {
                slot->setup_ran = true;
                return slot->setup;
            }
            return nullptr;
        }

        /// Marks |name|'s suite so its tests report failed without running.
        auto poison(std::string_view const name) -> void {
            auto *const slot = find_slot(suite_of(name));
            if (slot != nullptr) {
                slot->poisoned = true;
            }
        }

        /// Whether |name|'s suite setup failed. A suite with no slot was
        /// never poisoned, because nothing ran against it.
        [[nodiscard]]
        auto poisoned(std::string_view const name) const -> bool {
            auto const *const slot = find_slot(suite_of(name));
            if (slot == nullptr) {
                return false;
            }
            return slot->poisoned;
        }

        /// The TearDown hook to run after |name|'s test: its suite's last of
        /// the run. Asked exactly once per selected test, after `before()`;
        /// a suite's tests being disjoint in the section order does not
        /// matter, the count is what decides.
        auto after(std::string_view const name) -> SuiteHook const * {
            auto *const slot = find_slot(suite_of(name));
            if (slot == nullptr) {
                return nullptr;
            }
            if (slot->remaining > 0) {
                slot->remaining -= 1;
            }
            if (slot->remaining == 0) {
                return slot->teardown;
            }
            return nullptr;
        }

      private:
        /// One suite's progress through the run. The hooks are the section's
        /// records; the rest is what the run did so far.
        struct Slot {
            std::string_view suite{};
            SuiteHook const *setup{nullptr};
            SuiteHook const *teardown{nullptr};
            usize remaining{};
            bool setup_ran{false};
            bool poisoned{false};
        };

        /// A test's full name is `Suite.Name`, the suite a prefix without a
        /// dot. Matching goes segment by segment, not by prefix, so a suite
        /// named `Foo` does not pick up a suite named `Foo.Bar`'s tests.
        [[nodiscard]]
        static auto suite_of(std::string_view const name) -> std::string_view {
            auto const dot = name.find('.');
            if (dot == std::string_view::npos) {
                return name;
            }
            return name.substr(0, dot);
        }

        [[nodiscard]]
        auto find_slot(std::string_view const suite) const -> Slot const * {
            for (auto i = usize{0}; i < m_count; ++i) {
                if (m_slots[i].suite == suite) {
                    return &m_slots[i];
                }
            }
            return nullptr;
        }

        [[nodiscard]]
        auto find_slot(std::string_view const suite) -> Slot * {
            for (auto i = usize{0}; i < m_count; ++i) {
                if (m_slots[i].suite == suite) {
                    return &m_slots[i];
                }
            }
            return nullptr;
        }

        Slot m_slots[MAX_SUITES];
        usize m_count{};
        bool m_valid{true};
    };

    /// The lifecycle one `KTEST_F` adapter runs: construct the test's object
    /// at the static storage the macro reserved, SetUp, the body when SetUp
    /// left nothing broken, TearDown, destroy. Every failure — in SetUp, the
    /// body or TearDown — lands in |context|, so the runner's verdict covers
    /// the whole lifecycle and reads like a plain test's. The trailing
    /// |args| are the construction arguments: none for a `KTEST_F`, whose
    /// object only aggregates a default-constructible fixture, and the
    /// instantiation's table value for the parameterized macros, whose object
    /// derives from the suite's KTEST_VALUE_PARAM base beside the fixture.
    template <typename Test, typename Body, typename... Args>
    auto run_fixture(::hsien::Context &context, void *const storage, Body &&body, Args &&...args)
        -> void {
        BindScope const scope(context);
        auto *const test =
            std::construct_at(static_cast<Test *>(storage), std::forward<Args>(args)...);
        auto const failures_before_setup = context.failure_count();
        test->SetUp();
        auto *const state = ::ktest::internal::current_state();
        auto const setup_broke = state != nullptr && (state->skipped || context.failure_count() !=
                                                                            failures_before_setup);
        if (!setup_broke) {
            body(*test);
        }
        test->TearDown();
        std::destroy_at(test);
    }

} // namespace internal

} // namespace ktest

/// Defines one test of a fixture, named `Fixture.Name`. The body that follows
/// is a member function of `Fixture_Name_Test`, a class the macro derives from
/// the fixture; `this` is a fixture instance whose set_up() ran before the
/// body and whose tear_down() runs after it, however the body ended.
///
/// The instance is the static test object the macro defines beside the
/// record: one object per record, constant-initialized when the fixture is
/// constexpr-constructible and otherwise initialized in the image's
/// constructor pass — either way before any test runs, and never allocated.
/// Its members keep whatever the body wrote for as long as the image runs, so
/// a `repeat` pass re-runs the body on the state the last pass left; a test
/// that needs fresh state resets it in the body, or takes a fixture whose
/// set_up does.
///
/// The record goes to `.hsien.tests` like a `KTEST`'s, so the runner reports
/// the test exactly like a plain one. The stages run through the CRTP base's
/// run_set_up()/run_tear_down(), which resolve to the fixture's SetUp()/
/// TearDown() at compile time — no virtual table, no virtual destructor, and
/// the static instance is never deleted through the base.
///
/// The class and its body are file-scope with external linkage, as in
/// GoogleTest: two `KTEST_F`s that share a full name meet as a duplicate
/// symbol at link time, where two plain `KTEST`s reach the runner's duplicate
/// check. Both are an authoring bug; the name is the test's identity.
#define KTEST_F(fixture_class, name_token)                                                         \
    class fixture_class##_##name_token##_Test final: public fixture_class {                        \
      public:                                                                                      \
        void ktest_body();                                                                         \
        static auto invoke(::hsien::Context &context) -> void {                                    \
            ::ktest::internal::BindScope const scope(context);                                     \
            auto const failures_before = context.failure_count();                                  \
            instance.run_set_up();                                                                 \
            auto *const state = ::ktest::internal::current_state();                                \
            auto const setup_broke =                                                               \
                state != nullptr &&                                                                \
                (state->skipped || context.failure_count() != failures_before);                    \
            if (!setup_broke) {                                                                    \
                instance.ktest_body();                                                             \
            }                                                                                      \
            instance.run_tear_down();                                                              \
        }                                                                                          \
        static fixture_class##_##name_token##_Test instance;                                       \
        KTEST_RECORD_SECTION(".hsien.tests")                                                       \
        FORCE_USED static inline ::hsien::TestRecord const record = {                              \
            .name = #fixture_class "." #name_token,                                                \
            .invoke = &invoke,                                                                     \
            .location = ::std::source_location::current(),                                         \
        };                                                                                         \
    };                                                                                             \
    fixture_class##_##name_token##_Test fixture_class##_##name_token##_Test::instance{};           \
    void fixture_class##_##name_token##_Test::ktest_body()

/// Defines the function that runs once before the run's first `KTEST_F` of
/// |fixture_class|, whatever selection ran; the matching
/// `KTEST_SUITE_TEARDOWN` runs once after its last. The body is a plain
/// function — the suite has no per-test state to hang `this` on — and its
/// assertions frame into the hook's own context, so a failure there is
/// reported by name and leaves the suite's tests out. Naming a type that is
/// not a complete fixture is a compile error, which is what keeps the hook
/// and its `KTEST_F`s on one suite.
#define KTEST_SUITE_SETUP(fixture_class)                                                           \
    static_assert(sizeof(fixture_class) != 0, "KTEST_SUITE_SETUP names a fixture type");           \
    class fixture_class##_SuiteSetUp {                                                             \
      public:                                                                                      \
        static void body();                                                                        \
        static auto invoke(::hsien::Context &context) -> void {                                    \
            ::ktest::internal::BindScope const scope(context);                                     \
            body();                                                                                \
        }                                                                                          \
        KTEST_RECORD_SECTION(".ktest.suite_hooks")                                                 \
        FORCE_USED static inline ::ktest::internal::SuiteHook const hook = {                       \
            .name = #fixture_class ".SuiteSetUp",                                                  \
            .suite = #fixture_class,                                                               \
            .kind = ::ktest::internal::SuiteHookKind::SetUp,                                       \
            .invoke = &invoke,                                                                     \
        };                                                                                         \
    };                                                                                             \
    void fixture_class##_SuiteSetUp::body()

/// Defines the function that runs once after the run's last `KTEST_F` of
/// |fixture_class|. Runs however the suite went: after a pass, after failed
/// tests, and after a failed suite setup, which is what makes this the place
/// to undo KTEST_SUITE_SETUP.
#define KTEST_SUITE_TEARDOWN(fixture_class)                                                        \
    static_assert(sizeof(fixture_class) != 0, "KTEST_SUITE_TEARDOWN names a fixture type");        \
    class fixture_class##_SuiteTearDown {                                                          \
      public:                                                                                      \
        static void body();                                                                        \
        static auto invoke(::hsien::Context &context) -> void {                                    \
            ::ktest::internal::BindScope const scope(context);                                     \
            body();                                                                                \
        }                                                                                          \
        KTEST_RECORD_SECTION(".ktest.suite_hooks")                                                 \
        FORCE_USED static inline ::ktest::internal::SuiteHook const hook = {                       \
            .name = #fixture_class ".SuiteTearDown",                                               \
            .suite = #fixture_class,                                                               \
            .kind = ::ktest::internal::SuiteHookKind::TearDown,                                    \
            .invoke = &invoke,                                                                     \
        };                                                                                         \
    };                                                                                             \
    void fixture_class##_SuiteTearDown::body()

#endif // #ifndef KTEST_FIXTURE_HPP
