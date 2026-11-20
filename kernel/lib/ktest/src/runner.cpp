/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The `ktest` console command: walks the runtime's record section, applies a
// selection and a run plan (shuffle, repeat, shard), and runs each selected
// test through the runtime one at a time so it can frame and time it. Its
// lines are the host's contract: `RUN`, `OK`, `FAIL`, `FAILED`, `SKIP`,
// `TEST`, `ERROR`, the `SHUFFLE` line a shuffled run opens with, and the
// final `SUMMARY`.
#include <ktest/environment.hpp>
#include <ktest/filter.hpp>
#include <ktest/fixture.hpp>
#include <ktest/options.hpp>
#include <ktest/run_plan.hpp>
#include <ktest/run_state.hpp>
#include <ktest/session.hpp>
#include <ktest/summary.hpp>

#include <atomic>
#include <span>
#include <string_view>
#include <utility>

#include <gktl/command.hpp>
#include <hsien/section.hpp>
#include <logz4/log.hpp>
#include <ours/status.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

namespace ktest {

/// What a kernel run calls for a log line and a microsecond count. The methods
/// are callable on an instance, which is how Run holds one.
struct KernelServices {
    template <typename... Args>
    auto info(Args &&...args) const -> void {
        ::log::info(std::forward<Args>(args)...);
    }

    template <typename... Args>
    auto error(Args &&...args) const -> void {
        ::log::error(std::forward<Args>(args)...);
    }

    [[nodiscard]]
    auto now() const -> decltype(::ours::time::SteadyClock::now()) {
        return ::ours::time::SteadyClock::now();
    }

    template <typename Mark>
    [[nodiscard]]
    auto microseconds_since(Mark const started) const -> u64 {
        return static_cast<u64>(
            ::ours::duration_cast<::ours::Microseconds>(::ours::time::SteadyClock::now() - started)
                .count());
    }
};

/// One run for the image. Static because the shuffle buffer must not sit on
/// the kernel stack the test bodies share with this command.
Run<KernelServices> ACTIVE{KernelServices{}};

/// The suite hook section (kernel/lib/ktest's SuiteHook, emitted by
/// KTEST_SUITE_SETUP and KTEST_SUITE_TEARDOWN), bounded by the linker script
/// the way the hsien records are. Read on demand, so it lives in
/// `.data.rel.ro` and not in the .init tail.
extern "C" internal::SuiteHook ktest_suite_hooks_start[] asm("__ktest_suite_hooks_start");
extern "C" internal::SuiteHook ktest_suite_hooks_end[] asm("__ktest_suite_hooks_end");

/// The death-test section (kernel/lib/ktest's KTEST_DEATH), the same
/// hsien::TestRecord as the live tests but in a section of its own: the
/// live walk below never touches it, so `ktest all` and a filter run
/// cannot run a death test, and only the `death` selection reads here.
extern "C" ::hsien::TestRecord ktest_death_start[] asm("__ktest_death_start");
extern "C" ::hsien::TestRecord ktest_death_end[] asm("__ktest_death_end");

namespace {

    /// The thread the published test or stage runs on, for the wrong-thread
    /// guard. The stage runner publishes it around each hook and test through
    /// bind_stage_thread()/unbind_stage_thread(); only the runner thread
    /// writes it, but a worker thread that asserts by mistake reads it, so it
    /// is atomic: the read that is there to diagnose the mistake must not race
    /// the store.
    std::atomic<::ours::task::Thread *> OWNER_THREAD{nullptr};

    /// Leaves the run on every return from the command.
    struct LeaveRun final {
        LeaveRun() = default;
        ~LeaveRun() {
            ACTIVE.leave();
        }
        LeaveRun(LeaveRun const &) = delete;
        auto operator=(LeaveRun const &) -> LeaveRun & = delete;
    };

    /// Logs the one console line a rejection maps to, quoting the word that
    /// broke its rule where the line does. The texts are the host's
    /// contract: kshell_run.py judges a run by its error lines, and the
    /// change record pins them. The builder's BadValue is the runner's own
    /// three option faults, named by the option the word carried.
    auto report_rejection(gktl::Rejection const &rejection) -> void {
        switch (rejection.fault) {
            case gktl::OptionFault::UnknownOption:
            case gktl::OptionFault::BadChoice:
            case gktl::OptionFault::MissingRequired:
                // Unreachable: the words the declarations did not claim are
                // the tests', nothing is required, and no option takes
                // choices. They stand in the switch so a fault the builder
                // grows cannot pass silently.
                log::error("ktest: ERROR usage: ktest sel=<all|list|?|death|<filter>> "
                           "[filter=<glob>] [name=value ...]");
                return;
            case gktl::OptionFault::TooManyWords:
            case gktl::OptionFault::MalformedWord:
                log::error("ktest: ERROR usage: ktest sel=<all|list|?|death|<filter>> "
                           "[filter=<glob>] [name=value ...]");
                return;
            case gktl::OptionFault::BadValue:
                break;
        }
        if (rejection.name == "shuffle") {
            log::error("ktest: ERROR shuffle seed is not a number: \"{}\"", rejection.word);
        } else if (rejection.name == "repeat") {
            log::error("ktest: ERROR repeat is not a count of at least 1: \"{}\"", rejection.word);
        } else if (rejection.name == "shard") {
            log::error("ktest: ERROR shard is not M/N with 0 <= M < N: \"{}\"", rejection.word);
        } else {
            log::error("ktest: ERROR usage: ktest sel=<all|list|?|death|<filter>> "
                       "[filter=<glob>] [name=value ...]");
        }
    }

    /// The ktest command's declared options and the parse they answer.
    /// Namespace-scope rather than frame-resident because the whole test run
    /// happens inside this command's frame -- every test body nests below it
    /// on the same 16 KiB kernel stack -- and that stack should not carry
    /// what only one console command can use. The one-run-at-a-time guard
    /// (Run::enter) makes the storage exclusive, as it already does for the
    /// shuffle order.
    struct RunOptions {
        gktl::Options options{"ktest"};
        ustl::Result<gktl::Parsed, gktl::Rejection> planned{ustl::ok(gktl::Parsed{})};

        RunOptions() {
            options.add<std::string_view>("sel").help(
                "all, list, ?, death, or a filter glob naming the tests to run");
            options.add<std::string_view>("filter").help("the filter of a sel=death run");
            options.add<u64>("shuffle")
                .defaults_to(DEFAULT_SHUFFLE_SEED)
                .parse(&parse_seed)
                .help("run the selection in a shuffled order, with this seed");
            options.add<u64>("repeat").help("run the selection this many times");
            options.add<std::string_view>("shard").help("run only shard M of N, as shard=M/N");
            // Every word the declarations did not claim belongs to the tests.
            options.pass_through();
        }
    };

    RunOptions RUN_OPTIONS;

    auto ktest_command(gktl::Invocation const &invocation) -> int {
        if (!ACTIVE.enter()) {
            log::error("ktest: ERROR a test run is already in progress");
            return 1;
        }
        LeaveRun const leave;
        ACTIVE.adopt(RunPlan{});

        // The console bounds a command line at 16 words including the
        // command and the selection, so the words a run takes fit the fixed
        // array the plan records them into; a longer list is refused here,
        // with the count the line actually carried.
        auto const word_count = invocation.words().size();
        if (word_count > MAX_RUN_OPTIONS) {
            log::error("ktest: ERROR at most {} options, got {}", MAX_RUN_OPTIONS, word_count);
            return 1;
        }

        RUN_OPTIONS.planned = RUN_OPTIONS.options.parse(invocation);
        if (RUN_OPTIONS.planned.is_err()) {
            report_rejection(RUN_OPTIONS.planned.unwrap_err());
            return 1;
        }
        auto const &values = RUN_OPTIONS.planned.unwrap();

        // Which selection the line named. A bare `ktest` is still sel=all:
        // it is what triage types, and what the older notes in docs/ record.
        auto const selection =
            values.has("sel") ? values.get<std::string_view>("sel") : std::string_view{"all"};
        auto const all_selected = selection == "all";

        // The words besides the selection. words() carries the sel= word
        // too, and the list and death shapes are rules about what comes
        // with the selection, not about the selection itself.
        auto other_words = word_count;
        if (values.has("sel")) {
            other_words -= 1;
        }

        // The runner's own options belong to the all-selection alone: a run
        // that asked for a part of the registry asked for it as it stands.
        auto const runner_word =
            values.has("shuffle") || values.has("repeat") || values.has("shard");
        if (runner_word && !all_selected) {
            log::error("ktest: ERROR shuffle, repeat and shard need \"ktest sel=all\"");
            return 1;
        }

        // The list selections print the registry and take no other word.
        if (selection == "list" || selection == "?") {
            if (other_words != 0) {
                log::error("ktest: ERROR ktest sel=list takes no options");
                return 1;
            }
            return ACTIVE.list(hsien::section_begin(), hsien::section_end(), ktest_death_start,
                               ktest_death_end);
        }

        // The death selection is its own command shape: one boot per test,
        // the filter word its own, and nothing of the live run's plan. It
        // reads its own section, so a kernel without live tests still runs
        // its death tests.
        if (selection == "death") {
            auto const carries_filter = values.has("filter");
            if (other_words != (carries_filter ? 1 : 0) || runner_word) {
                log::error("ktest: ERROR usage: ktest sel=death [filter=<glob>]");
                return 1;
            }
            auto const *const begin = hsien::section_begin();
            auto const *const end = hsien::section_end();
            if (begin == end) {
                log::error("ktest: ERROR no tests are linked into this kernel");
                return 1;
            }
            if (ACTIVE.names_repeat(begin, end)) {
                return 1;
            }
            return ACTIVE.execute_death(ktest_death_start, ktest_death_end,
                                        carries_filter ? values.get<std::string_view>("filter")
                                                       : std::string_view{});
        }

        auto const *const begin = hsien::section_begin();
        auto const *const end = hsien::section_end();
        if (begin == end) {
            log::error("ktest: ERROR no tests are linked into this kernel");
            return 1;
        }
        // The plan: the runner's own words set its fields, and every word
        // the tests own is recorded for them, in the order given. A bare
        // number that means a count -- repeat -- is checked here, where its
        // word is at hand, rather than in the builder, whose shapes are the
        // console's, not the run's.
        auto built = RunPlan{};
        built.shuffle = values.has("shuffle");
        if (built.shuffle) {
            built.seed = values.get<u64>("shuffle");
        }
        if (values.has("repeat")) {
            auto const passes = values.get<u64>("repeat");
            if (passes == 0) {
                log::error("ktest: ERROR repeat is not a count of at least 1: \"0\"");
                return 1;
            }
            built.repeat = static_cast<usize>(passes);
        }
        if (values.has("shard")) {
            auto const word = values.get<std::string_view>("shard");
            auto const shard = Shard::parse(word);
            if (shard.is_err()) {
                log::error("ktest: ERROR shard is not M/N with 0 <= M < N: \"{}\"", word);
                return 1;
            }
            built.shard = shard.unwrap();
        }
        for (auto const &word: values.passthrough()) {
            built.options[built.option_count] = Option{.name = word.name, .value = word.value};
            built.option_count += 1;
        }
        ACTIVE.adopt(built);

        // The filter a run selects with is the sel word itself when it names
        // no reserved selection: ktest sel=Ktest.* runs what matches.
        auto const filter = all_selected ? std::string_view{} : selection;
        return ACTIVE.execute(begin, end, ktest_suite_hooks_start, ktest_suite_hooks_end, filter);
    }

} // namespace

namespace internal {

    auto current_state() -> RunState * {
        return CURRENT_STATE.load(std::memory_order_relaxed);
    }

    auto on_test_thread() -> bool {
        auto *const owner = OWNER_THREAD.load(std::memory_order_relaxed);
        return owner != nullptr && ::ours::task::Thread::Current::get() == owner;
    }

    auto framework_error(char const *const what) -> void {
        log::error("ktest: ERROR {}", what);
    }

    auto bind_stage_thread() -> void {
        OWNER_THREAD.store(::ours::task::Thread::Current::get(), std::memory_order_relaxed);
    }

    auto unbind_stage_thread() -> void {
        OWNER_THREAD.store(nullptr, std::memory_order_relaxed);
    }

    auto bind_context(::hsien::Context &context) -> void {
        auto *const state = CURRENT_STATE.load(std::memory_order_relaxed);
        if (state == nullptr) {
            framework_error("a test body ran with no test state published");
            return;
        }
        state->context = &context;
    }

    auto unbind_context() -> void {
        auto *const state = CURRENT_STATE.load(std::memory_order_relaxed);
        if (state != nullptr) {
            state->context = nullptr;
        }
    }

    auto record_skip(char const *const reason) -> void {
        auto *const state = CURRENT_STATE.load(std::memory_order_relaxed);
        if (state == nullptr) {
            framework_error("KTEST_SKIP ran with no test running");
            return;
        }
        if (!on_test_thread()) {
            framework_error("KTEST_SKIP ran on a thread that is not running the test");
            return;
        }
        // The runner prints the reason after the body has returned, so it
        // cannot keep the caller's pointer: copy it into the state, truncated
        // to fit.
        auto const text = std::string_view{reason};
        auto const room = sizeof(state->skip_reason) - 1;
        auto const count = text.size() < room ? text.size() : room;
        for (auto i = usize{0}; i < count; ++i) {
            state->skip_reason[i] = text[i];
        }
        state->skip_reason[count] = '\0';
        state->skipped = true;
    }

} // namespace internal

auto has_option(std::string_view const name) -> bool {
    return find_option(ACTIVE.plan().options, ACTIVE.plan().option_count, name) != nullptr;
}

auto option(std::string_view const name, std::string_view const fallback) -> std::string_view {
    auto const *const found = find_option(ACTIVE.plan().options, ACTIVE.plan().option_count, name);
    if (found == nullptr) {
        return fallback;
    }
    return found->value;
}

auto option_usize(std::string_view const name, usize const fallback) -> usize {
    auto const *const found = find_option(ACTIVE.plan().options, ACTIVE.plan().option_count, name);
    if (found == nullptr) {
        return fallback;
    }
    // The one digit primitive: an empty value and a non-number come out of
    // the same parse as different console lines, so the caller can say which
    // shape it saw.
    auto const parsed = parse_u64(found->value);
    if (parsed.is_err()) {
        if (found->value.empty()) {
            log::error("ktest: ERROR option \"{}\" needs a number", name);
        } else {
            log::error("ktest: ERROR option \"{}\" is not a number: \"{}\"", name, found->value);
        }
        return fallback;
    }
    return static_cast<usize>(parsed.unwrap());
}

} // namespace ktest

GKTL_COMMAND(Ktest, "ktest", ktest::ktest_command,
             "run the kernel tests: ktest [all|list|?|death|<filter>]");
