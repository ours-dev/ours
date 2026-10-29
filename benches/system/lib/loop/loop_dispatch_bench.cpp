/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host benchmark of what it costs to call a Loop slot's callback, three
/// ways, for docs/perf/261028/loop-dispatch.md:
///
///   A  the function pointer and the context loop::Callback is today:
///      slot.callback(slot.context, observed).
///   B  a virtual call on an abstract Waiter the slot points at:
///      slot.waiter->on_signals(observed).
///   C  a function pointer to a thunk the compiler writes for a member
///      function, with the object pointer beside it, which is what a
///      non-owning function reference is: slot.thunk(slot.object, observed).
///
/// Only the call differs. Each variant has a table of six slots, the Loop's
/// shape without its port, and each slot's target is a different class, as
/// the root has six kinds of registration, so the call site has six targets,
/// as Loop::run()'s has. The compiler cannot see which one a call goes to:
/// the order the slots are called in is read from a table at run time.
/// Every target does the same few instructions, a read of its own member and
/// two bit operations, and answers a loop::Action, so the three variants
/// return the same totals, which main() checks.
///
/// The order the slots are called in is the case:
///
///   same    always slot 0, so the call site has one target and the branch
///           predictor is right every time: the cost of the call itself;
///   cycle   slots 0 to 5 in turn: the predictor can learn it;
///   random  a fixed pseudo-random order over a table long enough that the
///           predictor cannot learn it: the call site has six targets and
///           guesses wrong now and then, as a server with six kinds of
///           client that talk in no pattern would make it.
///
/// Per case and variant it prints one sample per round:
///
///   ns/dispatch   host time per call of a callback, from one round of
///                 DISPATCHES calls. The host is an arm64 Mac, not the x86
///                 processors the kernel runs on, so the number says how the
///                 three compare on this processor, not what a call costs
///                 there.
///
/// Two yardsticks put the number in proportion: a direct call to a function
/// the compiler is told not to inline, which is the cost of calling at all;
/// and one write(2) of a byte to /dev/null, the cheapest real system call the
/// host has. A packet of the Loop's takes at least three system calls of the
/// kernel's own (port_wait, channel_read and channel_write), which no host
/// call stands for exactly; the write is only a scale.
#include <ours/signals.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <loop/loop.hpp>

namespace {
using loop::Action;
using ours::Signals;

/// Slots in each variant's table, and kinds of target.
constexpr usize SLOTS = 6;

/// Calls per round, a power of two. The host clock ticks every 41.7 ns on
/// Apple silicon, so a round has to last far longer than one call.
constexpr usize DISPATCHES = usize{1} << 22;

/// Rounds per case and variant: the statistics' n.
constexpr usize SAMPLES = 30;

/// How long the order table is, a power of two. 64 Ki entries is far past
/// anything the branch predictor keeps a history of.
constexpr usize ORDER_LENGTH = usize{1} << 16;

/// What the three variants are given as |observed|: any value will do, but
/// one the compiler cannot see.
constexpr auto OBSERVED = Signals::Readable | Signals::PeerClosed;

/// B's abstract class: what a slot would point at. Nothing is deleted
/// through it, so its destructor is not virtual, as in the kernel's own
/// interfaces of this kind.
class Waiter {
  public:
    virtual auto on_signals(Signals observed) -> Action = 0;

  protected:
    Waiter() = default;
    ~Waiter() = default;
};

/// C's reference: an object and a thunk that calls |Member| on it. This is
/// what the compiler writes for a function reference, and what a trampoline
/// written by hand for A is.
class Ref {
  public:
    template <typename T, auto Member>
    static auto bind(T &object) -> Ref {
        return Ref{&object, [](void *const pointer, Signals const observed) -> Action {
                       return (static_cast<T *>(pointer)->*Member)(observed);
                   }};
    }

    auto operator()(Signals const observed) const -> Action {
        return m_thunk(m_object, observed);
    }

  private:
    Ref(void *const object, auto (*thunk)(void *, Signals)->Action)
        : m_object(object), m_thunk(thunk) {}

    void *m_object;
    auto (*m_thunk)(void *, Signals) -> Action;
};

/// One kind of target. |Id| makes six different classes, so six different
/// functions and six vtables, which the linker cannot fold into one.
template <u32 Id>
class Target final: public Waiter {
  public:
    /// A: loop::Callback's shape.
    static auto callback(void *const context, Signals const observed) -> Action {
        return static_cast<Target *>(context)->judge(observed);
    }

    /// B: the virtual function.
    auto on_signals(Signals const observed) -> Action override {
        return judge(observed);
    }

    /// C: what the thunk calls.
    auto member(Signals const observed) -> Action {
        return judge(observed);
    }

    /// The same work for every kind: a read of its own member and two bit
    /// operations.
    [[nodiscard, gnu::always_inline]]
    auto judge(Signals const observed) const -> Action {
        auto const bits = static_cast<u32>(observed);
        return static_cast<Action>(((bits >> m_shift) ^ m_shift) & 1U);
    }

    /// The direct-call yardstick: A's callback, called by name, which the
    /// compiler is told not to inline.
    [[gnu::noinline]]
    static auto direct(void *const context, Signals const observed) -> Action {
        return callback(context, observed);
    }

  private:
    u32 m_shift = Id;
};

/// The six targets, one object of each kind.
struct Targets {
    Target<0> t0;
    Target<1> t1;
    Target<2> t2;
    Target<3> t3;
    Target<4> t4;
    Target<5> t5;
};

struct SlotA {
    loop::Callback callback;
    void *context;
};

struct SlotB {
    Waiter *waiter;
};

struct SlotC {
    Ref ref;
};

/// The three tables, each over the same six objects.
struct Tables {
    explicit Tables(Targets &targets)
        : a{{{&Target<0>::callback, &targets.t0},
             {&Target<1>::callback, &targets.t1},
             {&Target<2>::callback, &targets.t2},
             {&Target<3>::callback, &targets.t3},
             {&Target<4>::callback, &targets.t4},
             {&Target<5>::callback, &targets.t5}}}
        , b{{{&targets.t0},
             {&targets.t1},
             {&targets.t2},
             {&targets.t3},
             {&targets.t4},
             {&targets.t5}}}
        , c{{{Ref::bind<Target<0>, &Target<0>::member>(targets.t0)},
             {Ref::bind<Target<1>, &Target<1>::member>(targets.t1)},
             {Ref::bind<Target<2>, &Target<2>::member>(targets.t2)},
             {Ref::bind<Target<3>, &Target<3>::member>(targets.t3)},
             {Ref::bind<Target<4>, &Target<4>::member>(targets.t4)},
             {Ref::bind<Target<5>, &Target<5>::member>(targets.t5)}}} {}

    std::array<SlotA, SLOTS> a;
    std::array<SlotB, SLOTS> b;
    std::array<SlotC, SLOTS> c;
};

/// The three dispatch loops, each in a function of its own that the compiler
/// is told not to inline, so that each one's call site is what is timed. The
/// result is how many calls answered Close: the same for all three on one
/// order, which is what lets main() check that they did the same work.
[[gnu::noinline]]
auto dispatch_a(std::array<SlotA, SLOTS> const &slots, std::vector<u8> const &order,
                Signals const observed) -> u64 {
    auto closes = u64{0};
    for (auto i = usize{0}; i < DISPATCHES; ++i) {
        auto const &slot = slots[order[i & (ORDER_LENGTH - 1)]];
        closes += static_cast<u64>(slot.callback(slot.context, observed));
    }
    return closes;
}

[[gnu::noinline]]
auto dispatch_b(std::array<SlotB, SLOTS> const &slots, std::vector<u8> const &order,
                Signals const observed) -> u64 {
    auto closes = u64{0};
    for (auto i = usize{0}; i < DISPATCHES; ++i) {
        auto const &slot = slots[order[i & (ORDER_LENGTH - 1)]];
        closes += static_cast<u64>(slot.waiter->on_signals(observed));
    }
    return closes;
}

[[gnu::noinline]]
auto dispatch_c(std::array<SlotC, SLOTS> const &slots, std::vector<u8> const &order,
                Signals const observed) -> u64 {
    auto closes = u64{0};
    for (auto i = usize{0}; i < DISPATCHES; ++i) {
        auto const &slot = slots[order[i & (ORDER_LENGTH - 1)]];
        closes += static_cast<u64>(slot.ref(observed));
    }
    return closes;
}

/// The yardstick: the same loop, calling slot 0's function by name. The call
/// reads memory and nothing here changes it, so without the empty asm below
/// the compiler would see the same call with the same arguments every turn
/// and make it once: the first run of this bench measured 0.000 ns that way.
[[gnu::noinline]]
auto dispatch_direct(Targets &targets, Signals const observed) -> u64 {
    auto closes = u64{0};
    for (auto i = usize{0}; i < DISPATCHES; ++i) {
        asm volatile("" : : : "memory");
        closes += static_cast<u64>(Target<0>::direct(&targets.t0, observed));
    }
    return closes;
}

using Clock = std::chrono::steady_clock;

auto elapsed_ns(Clock::time_point const from, Clock::time_point const to) -> double {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
}

/// Keeps |value| alive to the end of the round, so that the loop that made it
/// is not removed.
auto keep(u64 const value) -> void {
    asm volatile("" : : "r"(value) : "memory");
}

/// The orders the slots are called in.
struct Orders {
    std::vector<u8> same = std::vector<u8>(ORDER_LENGTH, 0);
    std::vector<u8> cycle = std::vector<u8>(ORDER_LENGTH, 0);
    std::vector<u8> random = std::vector<u8>(ORDER_LENGTH, 0);

    Orders() {
        // xorshift64 with a fixed seed: the same order on every run.
        auto state = u64{0x9E3779B97F4A7C15};
        for (auto i = usize{0}; i < ORDER_LENGTH; ++i) {
            cycle[i] = static_cast<u8>(i % SLOTS);
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            random[i] = static_cast<u8>(state % SLOTS);
        }
    }
};

/// One raw sample: what was measured, what the number is, and the number.
struct Sample {
    std::string what;
    char const *metric;
    char const *unit;
    double value;
};

class Recorder {
  public:
    auto add(std::string const &what, char const *const metric, char const *const unit,
             double const value) -> void {
        m_samples.push_back({what, metric, unit, value});
    }

    /// The median of every sample of |what| and |metric|.
    [[nodiscard]]
    auto median(std::string const &what, char const *const metric) const -> double {
        std::vector<double> values;
        for (auto const &sample: m_samples) {
            if (sample.what == what && std::string(sample.metric) == metric) {
                values.push_back(sample.value);
            }
        }
        std::sort(values.begin(), values.end());
        if (values.empty()) {
            return 0;
        }
        auto const middle = values.size() / 2;
        if (values.size() % 2 == 1) {
            return values[middle];
        }
        return (values[middle - 1] + values[middle]) / 2;
    }

    /// One row per sample, in the format scripts/perf_stats.py reads.
    auto print_csv() const -> void {
        std::printf("case,metric,unit,value\n");
        for (auto const &sample: m_samples) {
            std::printf("%s,%s,%s,%.4f\n", sample.what.c_str(), sample.metric, sample.unit,
                        sample.value);
        }
    }

  private:
    std::vector<Sample> m_samples;
};

struct Pattern {
    char const *name;
    std::vector<u8> const *order;
};

/// Runs every pattern and variant |samples| times, the three variants one
/// after another within a round, so that a drift of the clock or the load
/// falls on all of them. Which variant goes first rotates from round to
/// round: A and C compile to the same dispatch loop, and the first run of
/// this bench, which always ran A first, put them 0.38 ns apart on the random
/// order, so a position in the round has a cost of its own. Returns false when
/// the variants did not do the same work.
auto measure(Recorder &recorder, Orders const &orders, Tables const &tables, Targets &targets,
             usize const samples) -> bool {
    auto const patterns = std::array<Pattern, 3>{{
        {"same", &orders.same},
        {"cycle", &orders.cycle},
        {"random", &orders.random},
    }};
    auto ok = true;
    for (auto round = usize{0}; round < samples; ++round) {
        for (auto const &pattern: patterns) {
            auto const &order = *pattern.order;
            auto totals = std::array<u64, 3>{};

            for (auto turn = usize{0}; turn < 3; ++turn) {
                auto const variant = (round + turn) % 3;
                auto const from = Clock::now();
                auto total = u64{0};
                if (variant == 0) {
                    total = dispatch_a(tables.a, order, OBSERVED);
                } else if (variant == 1) {
                    total = dispatch_b(tables.b, order, OBSERVED);
                } else {
                    total = dispatch_c(tables.c, order, OBSERVED);
                }
                auto const to = Clock::now();
                keep(total);
                totals[variant] = total;
                recorder.add(std::string(1, "ABC"[variant]) + " " + pattern.name, "ns/dispatch",
                             "ns", elapsed_ns(from, to) / static_cast<double>(DISPATCHES));
            }

            if (totals[0] != totals[1] || totals[1] != totals[2]) {
                std::fprintf(stderr, "the variants disagree on %s: A %llu, B %llu, C %llu\n",
                             pattern.name, static_cast<unsigned long long>(totals[0]),
                             static_cast<unsigned long long>(totals[1]),
                             static_cast<unsigned long long>(totals[2]));
                ok = false;
            }
        }

        auto const from = Clock::now();
        auto const direct = dispatch_direct(targets, OBSERVED);
        auto const to = Clock::now();
        keep(direct);
        recorder.add("yardstick direct call", "ns/dispatch", "ns",
                     elapsed_ns(from, to) / static_cast<double>(DISPATCHES));
    }
    return ok;
}

/// One write(2) of a byte to /dev/null, the cheapest real system call the
/// host has. The round is shorter than a dispatch round, since a call takes
/// far longer than a dispatch.
auto measure_syscall(Recorder &recorder, usize const samples) -> void {
    constexpr auto CALLS = usize{200000};
    auto const fd = ::open("/dev/null", O_WRONLY);
    if (fd < 0) {
        std::fprintf(stderr, "cannot open /dev/null\n");
        return;
    }
    char const byte = 'x';
    for (auto round = usize{0}; round < samples; ++round) {
        auto const from = Clock::now();
        for (auto i = usize{0}; i < CALLS; ++i) {
            auto const written = ::write(fd, &byte, 1);
            keep(static_cast<u64>(written));
        }
        auto const to = Clock::now();
        recorder.add("yardstick write(2) to /dev/null", "ns/call", "ns",
                     elapsed_ns(from, to) / static_cast<double>(CALLS));
    }
    ::close(fd);
}

auto print_row(Recorder const &recorder, char const *const pattern) -> void {
    auto const a = recorder.median(std::string("A ") + pattern, "ns/dispatch");
    auto const b = recorder.median(std::string("B ") + pattern, "ns/dispatch");
    auto const c = recorder.median(std::string("C ") + pattern, "ns/dispatch");
    std::printf("%-7s | %8.3f | %8.3f | %8.3f | %+8.3f | %+8.3f\n", pattern, a, b, c, b - a, c - a);
}
} // namespace

/// No argument: a summary of the medians. `csv`: every raw sample, for a
/// report. `check`: one short round, and whether the variants agree.
auto main(int argc, char **argv) -> int {
    auto const csv = argc == 2 && std::strcmp(argv[1], "csv") == 0;
    auto const check = argc == 2 && std::strcmp(argv[1], "check") == 0;
    if (argc != 1 && !csv && !check) {
        std::fprintf(stderr, "usage: loop-dispatch-bench [csv | check]\n");
        return 2;
    }

    Orders const orders;
    Targets targets;
    Tables const tables(targets);

    if (check) {
        Recorder scratch;
        auto const ok = measure(scratch, orders, tables, targets, 1);
        std::printf(ok ? "check ok\n" : "check FAILED\n");
        return ok ? 0 : 1;
    }

    // Once untimed first: a process that has just started may run on a
    // slower core, or at a lower clock, until the system sees it busy.
    Recorder warm_up;
    if (!measure(warm_up, orders, tables, targets, 3)) {
        return 1;
    }
    Recorder recorder;
    if (!measure(recorder, orders, tables, targets, SAMPLES)) {
        return 1;
    }
    measure_syscall(recorder, SAMPLES);

    if (csv) {
        std::printf("# loop-dispatch-bench csv: %zu samples a case, %zu calls a sample\n", SAMPLES,
                    DISPATCHES);
        recorder.print_csv();
        return 0;
    }

    std::printf("medians of %zu samples, %zu calls a sample\n", SAMPLES, DISPATCHES);
    std::printf("order   |  A ns    |  B ns    |  C ns    |  B - A   |  C - A\n");
    std::printf("--------+----------+----------+----------+----------+---------\n");
    print_row(recorder, "same");
    print_row(recorder, "cycle");
    print_row(recorder, "random");
    std::printf("\nyardsticks: a direct call, not inlined, %.3f ns; one write(2) of a byte to "
                "/dev/null, %.1f ns\n",
                recorder.median("yardstick direct call", "ns/dispatch"),
                recorder.median("yardstick write(2) to /dev/null", "ns/call"));
    return 0;
}
