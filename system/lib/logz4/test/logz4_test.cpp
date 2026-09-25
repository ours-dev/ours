/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/clock.hpp>
#include <logz4/domain.hpp>
#include <logz4/filter.hpp>
#include <logz4/log.hpp>
#include <logz4/ring.hpp>

#include <string>
#include <string_view>

#include <cassert>

namespace {
std::string g_emergency;

class CaptureLogger final: public logz4::Logger {
  public:
    auto write(char const *s, usize n) -> void override {
        text.append(s, n);
        ++writes;
    }

    std::string text;
    usize writes = 0;
};

constexpr auto &ALPHA = logz4::Domain::named<"alpha">;
constexpr auto &BETA = logz4::Domain::named<"beta">;

auto read_time() -> logz4::Time {
    return 42;
}

auto read_cpu() -> u32 {
    return 3;
}

auto reset() -> void {
    logz4::ring::reset_for_test();
    logz4::set_clock(nullptr);
    logz4::set_cpu(nullptr);
    logz4::set_min_level(logz4::Level::Trace);
    ALPHA.clear_min_level();
    BETA.clear_min_level();
    logz4::Domain::named<"kshell">.clear_min_level();
    logz4::current_domain_slot() = nullptr;
    g_emergency.clear();
}

// Two stand-ins for per-thread slots, for the test that swaps the hook.
logz4::Domain const *g_slot_a = nullptr;
logz4::Domain const *g_slot_b = nullptr;

auto slot_a() -> logz4::Domain const *& {
    return g_slot_a;
}

auto slot_b() -> logz4::Domain const *& {
    return g_slot_b;
}
} // namespace

auto logz4::emergency_write(char const *s, usize n) -> void {
    g_emergency.append(s, n);
}

auto main() -> int {
    CaptureLogger logger;
    logz4::init(&logger);

    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        log::info("hello {}", 7);
        assert(logger.writes == 1);
        assert(logger.text == "[info] hello 7\n");
    }

    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        logz4::set_clock(read_time);
        logz4::set_cpu(read_cpu);
        log::warn("x");
        assert(logger.text == "[42 cpu3 warn] x\n");
    }

    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        logz4::set_min_level(logz4::Level::Error);
        log::info("dropped");
        log::error("kept");
        assert(logger.text == "[error] kept\n");
        assert(logger.writes == 1);
    }

    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        log::info("{}", std::string(600, 'a'));
        assert(logger.writes == 1);
        assert(logger.text.find("...") != std::string::npos);
        assert(logger.text.back() == '\n');
        assert(logger.text.size() <= logz4::ring::LINE_BYTES);
    }

    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        log::info("one");
        log::warn("two");
        CaptureLogger dump;
        assert(logz4::ring::dump_to(dump) == 2);
        assert(dump.text == "[info] one\n[warn] two\n");
    }

    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        log::info("ringed");
        logger.text.clear();
        logz4::dump_recent();
        assert(logger.text == "[info] ringed\n");
    }

    {
        reset();
        class NullLike final: public logz4::Logger {
          public:
            auto write(char const *, usize) -> void override {}

            [[nodiscard]]
            auto is_null() const -> bool override {
                return true;
            }
        };
        NullLike null_like;
        logz4::set_global_logger(null_like);
        log::info("silent");
        log::error("boom");
        assert(g_emergency == "[error] boom\n");
        g_emergency.clear();
        logz4::dump_recent();
        assert(g_emergency.find("[error] boom\n") != std::string::npos);
    }

    // A domain prints its name after the bracket, and nothing else about the
    // line changes.
    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        ALPHA.info("hello {}", 7);
        assert(logger.writes == 1);
        assert(logger.text == "[info] (alpha) hello 7\n");
    }

    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        logz4::set_clock(read_time);
        logz4::set_cpu(read_cpu);
        ALPHA.warn("x");
        assert(logger.text == "[42 cpu3 warn] (alpha) x\n");
    }

    // One name, one object: a second mention of "alpha" is the same Domain,
    // so a floor set through either spelling governs both.
    {
        assert(&logz4::Domain::named<"alpha"> == &ALPHA);
        assert(&logz4::Domain::named<"beta"> != &ALPHA);
        assert(std::string_view(BETA.name()) == "beta");
    }

    // A domain follows the global floor until it is given its own.
    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        assert(!ALPHA.own_min_level().has_value());
        logz4::set_min_level(logz4::Level::Warn);
        assert(ALPHA.min_level() == logz4::Level::Warn);
        ALPHA.info("dropped");
        ALPHA.warn("kept");
        assert(logger.text == "[warn] (alpha) kept\n");
    }

    // Its own floor may be looser than the global one: that is the use --
    // one subsystem's debug output with everyone else held at error.
    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        logz4::set_min_level(logz4::Level::Error);
        ALPHA.set_min_level(logz4::Level::Debug);
        ALPHA.debug("alpha debug");
        ALPHA.trace("alpha trace");
        BETA.info("beta info");
        log::info("anonymous info");
        assert(logger.text == "[debug] (alpha) alpha debug\n");
        assert(ALPHA.own_min_level() == logz4::Level::Debug);
    }

    // ...or stricter, and clearing it hands the domain back to the global.
    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        ALPHA.set_min_level(logz4::Level::Error);
        ALPHA.warn("held back");
        BETA.warn("beta warn");
        ALPHA.clear_min_level();
        ALPHA.warn("let through");
        assert(logger.text == "[warn] (beta) beta warn\n[warn] (alpha) let through\n");
    }

    // Every domain is in the registry, found by the name it was declared
    // with, whether or not it has logged anything yet.
    {
        reset();
        assert(logz4::Domain::find("alpha") == &ALPHA);
        assert(logz4::Domain::find("beta") == &BETA);
        assert(logz4::Domain::find("alph") == nullptr);
        assert(logz4::Domain::find("") == nullptr);
        auto seen = usize{0};
        for (auto const &domain: logz4::Domain::all()) {
            assert(domain.name() != nullptr);
            ++seen;
        }
        // alpha, beta, and the kshell of the LogDomain tests below: a domain
        // named only as a LogDomain template argument is registered too.
        assert(seen == 3);
        assert(logz4::Domain::find("kshell") == &logz4::Domain::named<"kshell">);

        // What the console does with `log level alpha debug`.
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        logz4::set_min_level(logz4::Level::Error);
        logz4::Domain::find("alpha")->set_min_level(logz4::Level::Debug);
        ALPHA.debug("through the registry");
        assert(logger.text == "[debug] (alpha) through the registry\n");
    }

    // A ring dump can be narrowed to one domain; the full dump still
    // carries every line, anonymous ones included.
    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        ALPHA.info("a1");
        BETA.info("b1");
        log::info("n1");
        ALPHA.warn("a2");

        CaptureLogger only_alpha;
        assert(logz4::ring::dump_to(only_alpha, ALPHA) == 2);
        assert(only_alpha.text == "[info] (alpha) a1\n[warn] (alpha) a2\n");

        CaptureLogger all;
        assert(logz4::ring::dump_to(all) == 4);

        logger.text.clear();
        logz4::dump_recent(BETA);
        assert(logger.text == "[info] (beta) b1\n");
    }

    // A LogDomain scope puts the free functions' lines into its domain, and
    // leaving it puts them back -- the shape the feature was asked for in.
    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        {
            logz4::LogDomain<"kshell"> domain;
            log::trace("Heelo");
        }
        log::trace("Workd");
        assert(logger.text == "[trace] (kshell) Heelo\n[trace] Workd\n");
    }

    // Scopes nest; each one hands back the one it replaced.
    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        {
            logz4::LogDomain<"alpha"> outer;
            log::info("1");
            {
                logz4::LogDomain<"beta"> inner;
                log::info("2");
            }
            log::info("3");
        }
        log::info("4");
        assert(logger.text == "[info] (alpha) 1\n[info] (beta) 2\n[info] (alpha) 3\n[info] 4\n");
        assert(logz4::current_domain() == nullptr);
    }

    // A line in a scope is filtered at the scope's domain floor, and lands in
    // the ring under that domain.
    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        logz4::set_min_level(logz4::Level::Error);
        ALPHA.set_min_level(logz4::Level::Debug);
        {
            logz4::LogDomain<"alpha"> domain;
            log::debug("in scope");
        }
        log::debug("out of scope");
        assert(logger.text == "[debug] (alpha) in scope\n");

        CaptureLogger only_alpha;
        assert(logz4::ring::dump_to(only_alpha, ALPHA) == 1);
    }

    // A domain's own members name it outright, whatever scope they run in.
    {
        reset();
        logger = CaptureLogger{};
        logz4::set_global_logger(logger);
        {
            logz4::LogDomain<"alpha"> domain;
            BETA.info("explicit");
        }
        assert(logger.text == "[info] (beta) explicit\n");
    }

    // Leaving restores the slot the guard wrote on the way in, not whichever
    // slot the context has by then: the kernel installs its per-thread hook
    // while boot is already running, possibly inside a scope.
    {
        reset();
        logz4::set_domain_slot(slot_a);
        {
            logz4::LogDomain<"alpha"> domain;
            assert(g_slot_a == &ALPHA);
            logz4::set_domain_slot(slot_b);
            assert(logz4::current_domain() == nullptr);
        }
        assert(g_slot_a == nullptr);
        assert(g_slot_b == nullptr);
        // Leave a hook installed that reset() can clear; the image's own
        // default slot is private to domain.cpp.
        logz4::set_domain_slot(slot_a);
    }

    {
        auto level = logz4::Level::Info;
        assert(logz4::parse_level("trace", level));
        assert(level == logz4::Level::Trace);
        assert(logz4::parse_level("error", level));
        assert(level == logz4::Level::Error);
        assert(!logz4::parse_level("nope", level));
        assert(std::string_view(logz4::level_name(logz4::Level::Warn)) == "warn");
    }

    return 0;
}
