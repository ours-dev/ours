/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `steal.bench`: what the work-stealing knobs do to eight threads that start
// on one CPU, and to three or four (.agents/tasks/steal-policy-measure/
// DESIGN.md, workloads W1 and W2). A controller on the first online CPU starts
// the workers on the next one, lets the others halt, then allows the workers on
// the next three CPUs and wakes the two new ones with a reschedule request, as
// a wakeup would. The
// workers are a register-only loop, so how fast one runs does not depend on a
// cache. For a window of two seconds from that moment it watches where the
// workers are, and measures:
//
//   w1 settle    when the last worker changed CPU, from the moment they were
//                allowed to; none moved is 0. Milliseconds.
//   w1 settled   whether nothing moved for the last 50 ms of the window.
//   w1 moved     the threads the thieves took (schedule.steals), in all.
//   w1 spread    the most workers on one of the three CPUs less the fewest, at
//                the end. Three CPUs hold eight workers as 3/3/2, a spread of
//                one; nobody stealing leaves 8/0/0, a spread of eight. Three
//                workers spread one each (0) or not, four as 2/1/1 (1) or not.
//   w1 slowest   the progress of the slowest worker over the mean progress of
//                the eight: near 1 when each had its share of a CPU. A thread
//                runs in slices of 64 ms, and the window holds about 31 of
//                them, so even eight workers on one CPU read 0.8 to 1: only a
//                difference much larger than that is a difference.
//   w2 cpus      the iterations of all eight over the iterations of one worker
//                alone on one CPU in the same time: the number of CPUs' worth
//                of work done. One is no stealing; three is the three CPUs
//                kept busy, and a little less is what an emulator gives when
//                three of its CPUs run at once beside the controller's, which
//                the one worker alone does not pay.
//
// A worker's CPU is the one whose scheduler holds it, running or queued
// (Thread::curr_cpu()): a worker that was taken and waits its turn behind
// another on the thief is already counted there, which the CPU it last ran on
// would not say for as long as a slice. The field is written under the run
// queue's lock and read here without it, an aligned word that cannot be seen
// half written, so a poll sees a worker on the CPU it left or the one it
// joined; one taken between the two steps of a take reads as still on the
// victim, a poll late.
//
// The knobs are set by this command between rounds (StealPolicy::set()), so
// one boot gives samples of every configuration, interleaved so that a drift
// of the host falls on all of them. The first round is a warm-up: it runs and
// prints its line and is not in the raw rows. The knobs are put back as they
// were.
//
//   steal.bench [all] [csv] [rounds=N]
//
// `csv` also prints every raw sample, one line each, as `steal.bench: csv:
// case,metric,unit,value`: the rows a performance report keeps
// (docs/perf/TEMPLATE.md). The case is the configuration, with `3w` or `4w` in
// front of it when the workers are three or four and not eight. `rounds` is
// the number of rounds, seven if not given, the warm-up included: six rows of
// each metric and configuration a boot, thirty over five boots. A round of one
// configuration takes 2.7 s of waiting and there are fifteen, so the default is
// about 284 s, longer than `kshell_run.py`'s default command timeout: pass
// `--command-timeout 600`. One round is the warm-up alone and prints no rows.
//
// Written for a machine of four CPUs: the controller's, which holds no worker
// and is in the mean the thieves reach, and the three of the workers. That mean
// is why eight workers end 4/2/2 and not 3/3/2. On more CPUs the ones that hold
// no worker are in it too, and the numbers are another machine's.
//
// The workers' records share cache lines, which a real processor would charge
// to the configurations that spread them over CPUs and an emulator does not
// model. That is for the run on hardware, which is not this one.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/ipi_service.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/sched/domains.hpp>
#include <ours/task/sched/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>
#include <string_view>

#include <bench/run.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using sched_domain::Knob;
    using task::Thread;
    using task::sched::Scheduler;
    using task::sched::StealPolicy;
    using time::SteadyClock;

    /// The most workers a configuration starts.
    constexpr usize MAX_WORKERS = 8;
    constexpr usize WORKER_CPUS = 3;
    /// The controller's CPU and the workers'.
    constexpr usize CPUS_NEEDED = WORKER_CPUS + 1;

    /// The window the workers are watched for, and the time one worker alone
    /// is run for to see what a CPU gives it: a quarter of the window, scaled
    /// up. Whole milliseconds, so that the scale is exact.
    constexpr usize WINDOW_MS = 2000;
    constexpr usize CALIBRATION_MS = 500;
    static_assert(WINDOW_MS % CALIBRATION_MS == 0);

    /// Long enough for the workers to be queued behind the one running and for
    /// the other CPUs to have halted.
    constexpr auto SETTLE = Milliseconds(100);
    /// How long nothing may move for the window to end settled.
    constexpr auto STABLE_FOR = Milliseconds(50);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    constexpr auto JOIN_WITHIN = Seconds(30);

    constexpr usize DEFAULT_ROUNDS = 7;
    constexpr usize MAX_ROUNDS = 1000;
    constexpr usize MAX_DIGITS = 4;

    /// Steps of the loop between two counts of it. A count is the unit of work
    /// the figures are made of.
    constexpr usize CHUNK = 2000;

    /// The metrics, in the order a sample prints them.
    constexpr char const *METRICS[] = {"w1 settle", "w1 settled", "w1 moved",
                                       "w1 spread", "w1 slowest", "w2 cpus"};

    /// One configuration: the knobs it sets away from their defaults.
    struct Config {
        char const *name;
        /// How many workers start on the first CPU.
        usize workers;
        usize count;
        Knob knobs[2];
        usize values[2];
    };

    /// Stealing off is the negative control: it must show no balance. A batch
    /// of one stands for the old behaviour of one thread per look.
    ///
    /// Eight workers queue deeper than any threshold swept, so the thresholds
    /// show only on a queue of about their depth: three or four workers, where
    /// the run queue holds as many threads, the one running counted, as a
    /// threshold asks for. A batch of more than one cannot show there, since a
    /// look takes the one thread the mean leaves room for.
    constexpr Config CONFIGS[] = {
        {"off", 8, 1, {Knob::Enabled, Knob::Enabled}, {0, 0}},
        {"default", 8, 0, {Knob::Enabled, Knob::Enabled}, {0, 0}},
        {"batch1", 8, 2, {Knob::MaxBatchSameNode, Knob::MaxBatchOtherNode}, {1, 1}},
        {"batch2", 8, 2, {Knob::MaxBatchSameNode, Knob::MaxBatchOtherNode}, {2, 2}},
        {"batch8", 8, 2, {Knob::MaxBatchSameNode, Knob::MaxBatchOtherNode}, {8, 8}},
        {"min3", 8, 1, {Knob::MinRunnableSameNode, Knob::MinRunnableSameNode}, {3, 3}},
        {"min4", 8, 1, {Knob::MinRunnableSameNode, Knob::MinRunnableSameNode}, {4, 4}},
        {"3w off", 3, 1, {Knob::Enabled, Knob::Enabled}, {0, 0}},
        {"3w default", 3, 0, {Knob::Enabled, Knob::Enabled}, {0, 0}},
        {"3w min3", 3, 1, {Knob::MinRunnableSameNode, Knob::MinRunnableSameNode}, {3, 3}},
        {"3w min4", 3, 1, {Knob::MinRunnableSameNode, Knob::MinRunnableSameNode}, {4, 4}},
        {"4w off", 4, 1, {Knob::Enabled, Knob::Enabled}, {0, 0}},
        {"4w default", 4, 0, {Knob::Enabled, Knob::Enabled}, {0, 0}},
        {"4w min3", 4, 1, {Knob::MinRunnableSameNode, Knob::MinRunnableSameNode}, {3, 3}},
        {"4w min4", 4, 1, {Knob::MinRunnableSameNode, Knob::MinRunnableSameNode}, {4, 4}},
    };

    constexpr auto workers_fit() -> bool {
        for (auto const &config: CONFIGS) {
            if (config.workers == 0 || config.workers > MAX_WORKERS) {
                return false;
            }
        }
        return true;
    }
    static_assert(workers_fit());

    /// The knobs as they were, put back when the command ends.
    class SavedKnobs {
      public:
        SavedKnobs() {
            for (usize i = 0; i < sched_domain::NUM_KNOBS; ++i) {
                m_values[i] = StealPolicy::get(static_cast<Knob>(i));
            }
        }

        ~SavedKnobs() {
            for (usize i = 0; i < sched_domain::NUM_KNOBS; ++i) {
                // A value that was in range when it was read is in range now.
                auto _ = StealPolicy::set(static_cast<Knob>(i), m_values[i]);
            }
        }

        SavedKnobs(SavedKnobs const &) = delete;
        auto operator=(SavedKnobs const &) -> SavedKnobs & = delete;

      private:
        usize m_values[sched_domain::NUM_KNOBS]{};
    };

    /// Every knob to its default, then the configuration's own. False if a
    /// knob refused a value, which would make the round another configuration.
    [[nodiscard]]
    auto apply(Config const &config) -> bool {
        auto ok = true;
        for (usize i = 0; i < sched_domain::NUM_KNOBS; ++i) {
            auto const knob = static_cast<Knob>(i);
            ok &= StealPolicy::set(knob, sched_domain::knob_info(knob).default_value);
        }
        for (usize i = 0; i < config.count; ++i) {
            ok &= StealPolicy::set(config.knobs[i], config.values[i]);
        }
        return ok;
    }

    /// The thread that runs the command kept on one CPU for a scope: the
    /// controller's CPU is not one the workers are allowed on.
    class Pinned {
      public:
        explicit Pinned(CpuNum const cpu) {
            Thread::Current::get()->set_cpu_affinity_hard(CpuMask::of(cpu));
            // The thread moves at its next reschedule, which the sleep is.
            auto _ = Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }

        ~Pinned() {
            Thread::Current::get()->set_cpu_affinity_hard(cpu_all_mask());
        }

        Pinned(Pinned const &) = delete;
        auto operator=(Pinned const &) -> Pinned & = delete;
    };

    struct Worker {
        std::atomic<bool> const *stop = nullptr;
        /// Counts of CHUNK steps. Written by the worker alone.
        std::atomic<u64> iterations{0};
        /// Written and never read: it keeps the loop from being optimised
        /// away.
        std::atomic<u64> sink{0};
    };

    struct State {
        std::atomic<bool> stop{false};
        Worker workers[MAX_WORKERS];

        State() {
            for (auto &worker: workers) {
                worker.stop = &stop;
            }
        }
    };

    auto worker_routine(void *const arg) -> i32 {
        auto &worker = *static_cast<Worker *>(arg);
        u64 x = 0x9e3779b97f4a7c15ULL;
        while (!worker.stop->load(std::memory_order_acquire)) {
            for (usize i = 0; i < CHUNK; ++i) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
            }
            worker.sink.store(x, std::memory_order_relaxed);
            worker.iterations.store(worker.iterations.load(std::memory_order_relaxed) + 1,
                                    std::memory_order_relaxed);
        }
        return 0;
    }

    /// Some of the workers, started pinned to one CPU and later allowed on
    /// more. The state is on the heap and freed only once every worker is
    /// joined, so a worker never reads a state that is gone.
    class Crew {
      public:
        Crew() = default;

        ~Crew() {
            // A worker that did not join leaves its state behind, and there is
            // nothing more to do about it here.
            auto _ = finish();
        }

        Crew(Crew const &) = delete;
        auto operator=(Crew const &) -> Crew & = delete;

        /// |count| workers, each allowed on |cpu| alone, running.
        [[nodiscard]]
        auto start(usize const count, CpuNum const cpu) -> bool {
            m_state = new (mm::ALLOC_KERNEL) State();
            if (m_state == nullptr) {
                return false;
            }
            for (usize i = 0; i < count; ++i) {
                auto *const thread = Thread::spawn("steal.bench/worker", worker_routine,
                                                   &m_state->workers[i], task::DEFAULT_PRIORITY);
                if (thread == nullptr) {
                    return false;
                }
                m_threads[i] = thread;
                m_count = i + 1;
                thread->set_cpu_affinity_hard(CpuMask::of(cpu));
                // The thread is new, so it cannot already be running:
                // resume() only says whether it was.
                auto _ = thread->resume();
            }
            return true;
        }

        auto allow(CpuMask const &cpus) -> void {
            for (usize i = 0; i < m_count; ++i) {
                m_threads[i]->set_cpu_affinity_hard(cpus);
            }
        }

        /// The CPU whose scheduler holds worker |i|; the header says how it is
        /// read.
        [[nodiscard]]
        auto cpu_of(usize const i) const -> CpuNum {
            return m_threads[i]->curr_cpu();
        }

        [[nodiscard]]
        auto iterations(usize const i) const -> u64 {
            return m_state->workers[i].iterations.load(std::memory_order_relaxed);
        }

        [[nodiscard]]
        auto count() const -> usize {
            return m_count;
        }

        /// Stops the workers and joins them. False if one did not join, and
        /// then the state is left to it.
        [[nodiscard]]
        auto finish() -> bool {
            if (m_state == nullptr) {
                return true;
            }
            m_state->stop.store(true, std::memory_order_release);
            auto joined = true;
            for (usize i = 0; i < m_count; ++i) {
                joined &= m_threads[i]->join(
                              nullptr, time::Deadline::at(SteadyClock::now() + JOIN_WITHIN)) ==
                          Status::Ok;
            }
            m_count = 0;
            if (joined) {
                delete m_state;
            }
            m_state = nullptr;
            return joined;
        }

      private:
        State *m_state = nullptr;
        Thread *m_threads[MAX_WORKERS]{};
        usize m_count = 0;
    };

    /// What one round saw.
    struct Sample {
        bool settled = false;
        u64 settle_us = 0;
        u64 moved = 0;
        usize per_cpu[WORKER_CPUS]{};
        usize spread = 0;
        /// Thousandths.
        u64 slowest_milli = 0;
        u64 cpus_milli = 0;
    };

    auto sleep_for(Duration const duration) -> void {
        // The result says only whether the sleep was cut short, and the
        // interruptible mode is off, so it cannot be.
        auto _ = Thread::Current::sleep_for(duration, task::Interruptible::No);
    }

    /// The iterations of one worker alone on |cpu| over the window: what a CPU
    /// gives a worker.
    [[nodiscard]]
    auto one_worker_alone(CpuNum const cpu) -> u64 {
        Crew crew;
        if (!crew.start(1, cpu)) {
            return 0;
        }
        sleep_for(SETTLE);
        auto const before = crew.iterations(0);
        sleep_for(Milliseconds(CALIBRATION_MS));
        auto const done = crew.iterations(0) - before;
        if (!crew.finish()) {
            return 0;
        }
        return done * (WINDOW_MS / CALIBRATION_MS);
    }

    [[nodiscard]]
    auto steals_in_all(CpuNum const (&cpus)[CPUS_NEEDED]) -> u64 {
        u64 sum = 0;
        for (auto const cpu: cpus) {
            sum += Scheduler::Current::num_steals(cpu);
        }
        return sum;
    }

    /// One round of |config|. |cpus| holds the controller's CPU, then the
    /// workers'. False if the round could not be set up or a worker did not
    /// join; |out| is then not a sample.
    [[nodiscard]]
    auto run_round(CpuNum const (&cpus)[CPUS_NEEDED], Config const &config, Sample &out) -> bool {
        if (!apply(config)) {
            log::error("steal.bench: {}: a knob refused its value", config.name);
            return false;
        }
        auto const alone = one_worker_alone(cpus[1]);
        if (alone == 0) {
            log::error("steal.bench: {}: the calibration worker did not run", config.name);
            return false;
        }

        Crew crew;
        auto const workers = config.workers;
        if (!crew.start(workers, cpus[1])) {
            log::error("steal.bench: {}: the workers did not start", config.name);
            return false;
        }
        sleep_for(SETTLE);
        for (usize i = 0; i < workers; ++i) {
            if (crew.cpu_of(i) != cpus[1]) {
                log::error("steal.bench: {}: worker {} is on CPU {}, not CPU {}", config.name, i,
                           crew.cpu_of(i), cpus[1]);
                return false;
            }
        }

        // The moment they are allowed to move.
        u64 before[MAX_WORKERS];
        for (usize i = 0; i < workers; ++i) {
            before[i] = crew.iterations(i);
        }
        auto const steals_before = steals_in_all(cpus);
        auto const started = SteadyClock::now();
        CpuMask allowed;
        for (usize i = 1; i < CPUS_NEEDED; ++i) {
            allowed.set(cpus[i]);
        }
        crew.allow(allowed);
        for (usize i = 2; i < CPUS_NEEDED; ++i) {
            IpiService::send_resched(cpus[i]);
        }

        CpuNum seen[MAX_WORKERS];
        for (usize i = 0; i < workers; ++i) {
            seen[i] = cpus[1];
        }
        auto last_change = started;
        auto const ends = started + Milliseconds(WINDOW_MS);
        auto now = started;
        while (now < ends) {
            sleep_for(POLL_INTERVAL);
            now = SteadyClock::now();
            for (usize i = 0; i < workers; ++i) {
                auto const where = crew.cpu_of(i);
                if (where != seen[i]) {
                    seen[i] = where;
                    last_change = now;
                }
            }
        }

        // Read before the workers stop, so that stopping them is not in the
        // window.
        u64 progress[MAX_WORKERS];
        u64 total = 0;
        u64 slowest = 0;
        for (usize i = 0; i < workers; ++i) {
            progress[i] = crew.iterations(i) - before[i];
            total += progress[i];
            if (i == 0 || progress[i] < slowest) {
                slowest = progress[i];
            }
        }
        auto const steals_after = steals_in_all(cpus);
        if (!crew.finish()) {
            log::error("steal.bench: {}: a worker did not join", config.name);
            return false;
        }
        if (total == 0) {
            log::error("steal.bench: {}: the workers did not run", config.name);
            return false;
        }

        out = Sample{};
        out.moved = steals_after - steals_before;
        out.settle_us = static_cast<u64>((last_change - started).count() / 1000);
        out.settled = (now - last_change) >= STABLE_FOR;
        for (usize i = 0; i < workers; ++i) {
            for (usize c = 0; c < WORKER_CPUS; ++c) {
                if (seen[i] == cpus[1 + c]) {
                    out.per_cpu[c] += 1;
                }
            }
        }
        usize most = out.per_cpu[0];
        usize fewest = out.per_cpu[0];
        for (auto const held: out.per_cpu) {
            if (held > most) {
                most = held;
            }
            if (held < fewest) {
                fewest = held;
            }
        }
        out.spread = most - fewest;
        out.slowest_milli = slowest * workers * 1000 / total;
        out.cpus_milli = total * 1000 / alone;
        return true;
    }

    /// Where the run's lines go, and whether its raw samples do too.
    struct Output {
        bench::Run const &run;
        bool csv;

        /// One raw sample; |milli| is the value in thousandths.
        auto sample(char const *const config, char const *const metric, char const *const unit,
                    u64 const milli) const -> void {
            if (csv) {
                log::info("{}: csv: {},{},{},{}.{:03}", run.name(), config, metric, unit,
                          milli / 1000, milli % 1000);
            }
        }
    };

    auto report(Output const &out, usize const round, Config const &config, Sample const &s)
        -> void {
        auto settled = "no";
        if (s.settled) {
            settled = "yes";
        }
        log::info("{}: round {} {}: settle {}.{:03} ms (settled: {}), moved {}, per CPU {}/{}/{}, "
                  "slowest {}.{:03} of the mean, {}.{:03} CPUs",
                  out.run.name(), round, config.name, s.settle_us / 1000, s.settle_us % 1000,
                  settled, s.moved, s.per_cpu[0], s.per_cpu[1], s.per_cpu[2],
                  s.slowest_milli / 1000, s.slowest_milli % 1000, s.cpus_milli / 1000,
                  s.cpus_milli % 1000);
        if (round == 0) {
            return;
        }
        auto settled_flag = u64{0};
        if (s.settled) {
            settled_flag = 1;
        }
        out.sample(config.name, "w1 settle", "ms", s.settle_us);
        out.sample(config.name, "w1 settled", "flag", settled_flag * 1000);
        out.sample(config.name, "w1 moved", "threads", s.moved * 1000);
        out.sample(config.name, "w1 spread", "workers", s.spread * 1000);
        out.sample(config.name, "w1 slowest", "share", s.slowest_milli);
        out.sample(config.name, "w2 cpus", "cpus", s.cpus_milli);
    }

    /// |text| as a decimal number up to MAX_DIGITS digits, or 0.
    auto parse_count(std::string_view const text) -> usize {
        if (text.empty() || text.size() > MAX_DIGITS) {
            return 0;
        }
        usize value = 0;
        for (auto const c: text) {
            if (c < '0' || c > '9') {
                return 0;
            }
            value = value * 10 + static_cast<usize>(c - '0');
        }
        return value;
    }

    auto steal_bench(int const argc, char **const argv) -> int {
        // `csv` and `rounds=N` are this benchmark's own, and `all` is
        // bench::Run's.
        auto csv = false;
        // bench::Run is given `all` as it was typed, and nothing else.
        auto run_argc = 1;
        auto all_index = 0;
        auto rounds = DEFAULT_ROUNDS;
        auto arguments_ok = true;
        for (int i = 1; i < argc; ++i) {
            auto const argument = std::string_view(argv[i]);
            if (argument == "csv") {
                csv = true;
            } else if (argument == "all") {
                run_argc = 2;
                all_index = i;
            } else if (argument.starts_with("rounds=")) {
                rounds = parse_count(argument.substr(7));
                if (rounds == 0 || rounds > MAX_ROUNDS) {
                    arguments_ok = false;
                }
            } else {
                arguments_ok = false;
            }
        }
        if (!arguments_ok) {
            log::warn("usage: steal.bench [all] [csv] [rounds=N]");
            return 1;
        }
        char *run_argv[] = {argv[0], argv[all_index]};
        auto const run = bench::Run("steal.bench", run_argc, run_argv);
        auto const out = Output{.run = run, .csv = csv};

        CpuNum cpus[CPUS_NEEDED];
        usize found = 0;
        Cpu::online_mask().for_each([&cpus, &found](CpuNum const cpu) {
            if (found < CPUS_NEEDED) {
                cpus[found++] = cpu;
            }
        });
        if (found < CPUS_NEEDED) {
            log::info("steal.bench: skip  needs {} online CPUs, {} are", CPUS_NEEDED, found);
            return run.finish();
        }
        if (Cpu::online_mask().count() != CPUS_NEEDED) {
            log::warn("steal.bench: written for a machine of {} CPUs; this one has {}, and the "
                      "CPUs that hold no worker are in the mean the thieves reach",
                      CPUS_NEEDED, Cpu::online_mask().count());
        }

        // Counts, and times of work an emulator does too: relative figures that
        // mean the same on any machine.
        for (auto const *const metric: METRICS) {
            if (!run.measures(metric, bench::Needs::AnyMachine)) {
                return run.finish();
            }
        }
        if (csv && rounds == 1) {
            log::warn("steal.bench: one round is the warm-up alone, and prints no raw rows");
        }

        SavedKnobs const saved;
        Pinned const pinned{cpus[0]};
        for (usize round = 0; round < rounds; ++round) {
            for (auto const &config: CONFIGS) {
                Sample sample;
                if (!run_round(cpus, config, sample)) {
                    return 1;
                }
                report(out, round, config, sample);
            }
        }
        return run.finish();
    }
} // namespace
} // namespace ours

GKTL_COMMAND(StealBench, "steal.bench", ours::steal_bench,
             "steal.bench [all] [csv] [rounds=N]: where eight threads end up and how much work "
             "gets done under each work-stealing setting");
