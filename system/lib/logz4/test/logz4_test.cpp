/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <string>
#include <string_view>

#include <cassert>

#include <logz4/clock.hpp>
#include <logz4/filter.hpp>
#include <logz4/log.hpp>
#include <logz4/ring.hpp>

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
    g_emergency.clear();
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
