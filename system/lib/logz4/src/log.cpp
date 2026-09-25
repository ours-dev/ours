/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/clock.hpp>
#include <logz4/domain.hpp>
#include <logz4/log.hpp>
#include <logz4/ring.hpp>

#include <cstring>

#include <ustl/fmt/format_to.hpp>

namespace logz4 {
namespace {
    /// Cursor over one line under assembly, on the caller's stack. Nothing
    /// here is shared between CPUs, which is the point: the buffer this
    /// replaced was a single static array that every CPU formatted into
    /// simultaneously.
    class LineBuilder final {
      public:
        LineBuilder(char *buffer, usize capacity) : m_cursor(buffer), m_end(buffer + capacity) {}

        auto append(char const *text, usize n) -> void {
            for (usize i = 0; i < n; ++i) {
                if (m_cursor == m_end) {
                    return;
                }
                *m_cursor++ = text[i];
            }
        }

        auto append(char const *text) -> void {
            append(text, std::strlen(text));
        }

        auto append(char c) -> void {
            if (m_cursor != m_end) {
                *m_cursor++ = c;
            }
        }

        auto append_decimal(u64 value) -> void {
            char digits[20];
            usize count = 0;
            do {
                digits[count++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);
            while (count != 0) {
                append(digits[--count]);
            }
        }

        [[nodiscard]]
        auto cursor() -> char * {
            return m_cursor;
        }

        [[nodiscard]]
        auto remaining() const -> usize {
            return static_cast<usize>(m_end - m_cursor);
        }

      private:
        char *m_cursor;
        char *m_end;
    };

    /// A bounded output iterator for std::vformat_to.
    ///
    /// vformat_to writes as much as the format string demands, and a bare
    /// `char *` tells it nothing about where the buffer ends -- one long
    /// log line would run straight off the end of the line buffer. Bytes
    /// past the limit are dropped and remembered, so the caller can end a
    /// cut line with an explicit ellipsis instead of leaving it looking
    /// complete.
    class BoundedOutput final {
      public:
        using difference_type = std::ptrdiff_t;

        BoundedOutput(char *first, char *last) : m_cursor(first), m_last(last) {}

        auto operator*() -> BoundedOutput & {
            return *this;
        }
        auto operator++() -> BoundedOutput & {
            return *this;
        }
        auto operator++(int) -> BoundedOutput {
            return *this;
        }

        auto operator=(char c) -> BoundedOutput & {
            if (m_cursor < m_last) {
                *m_cursor++ = c;
            } else {
                m_dropped = true;
            }
            return *this;
        }

        [[nodiscard]]
        auto cursor() const -> char * {
            return m_cursor;
        }

        [[nodiscard]]
        auto dropped() const -> bool {
            return m_dropped;
        }

      private:
        char *m_cursor;
        char *m_last;
        bool m_dropped = false;
    };

    /// First field is Time (when the line happened), then cpu, then
    /// severity — the same shape as Android printk. The integer is printed
    /// as-is; converting units is the clock's job, not ours.
    ///
    /// The domain follows the bracket, in parentheses: `[warn] (kshell) ...`.
    /// Outside the bracket because it is not metadata about the moment the
    /// way time, cpu and level are, but the start of what was said -- where
    /// Linux's pr_fmt puts its "kshell: " prefix, which is what call sites
    /// in this tree wrote by hand before domains existed. A line logged
    /// outside every LogDomain prints no name, so it looks exactly as it
    /// did before domains.
    auto write_prefix(LineBuilder &line, Time time, bool have_time, u32 cpu, bool have_cpu,
                      Level level, Domain const *domain) -> void {
        line.append("[");
        if (have_time) {
            line.append_decimal(time);
            line.append(" ");
        }
        if (have_cpu) {
            line.append("cpu");
            line.append_decimal(cpu);
            line.append(" ");
        }
        line.append(level_name(level));
        line.append("] ");
        if (domain != nullptr) {
            line.append("(");
            line.append(domain->name());
            line.append(") ");
        }
    }
} // namespace

template <typename FormatString>
auto vlog(Logger &logger, Level level, FormatString fmt, ustl::fmt::FormatArgs const &args)
    -> void {
    // Whatever LogDomain scope the running context is in, if any. Read here,
    // once per line, and not by the caller: the slot belongs to physload in
    // a phys image (see current_domain_slot()), and this is physload's code.
    auto const *const domain = current_domain();

    // The one level check on the path: a line in a domain filters at that
    // domain's floor (which may be looser than the global one), any other
    // line at the global floor. It stays here, behind the vlog_dispatch()
    // call, rather than being hoisted inline into the free functions: a phys
    // module has its own hidden copy of the inline global floor, which nobody
    // sets -- the boot option lands in physload's -- so an inline check there
    // would filter module lines against the compile-time default instead.
    if (domain != nullptr) {
        if (!domain->is_enabled(level)) {
            return;
        }
    } else if (!is_enabled(level)) {
        return;
    }

    auto time = Time{0};
    auto cpu_id = u32{0};
    auto have_time = false;
    auto have_cpu = false;
    if (auto const read_time = clock(); read_time != nullptr) {
        time = read_time();
        have_time = true;
    }
    if (auto const read_cpu = cpu(); read_cpu != nullptr) {
        cpu_id = read_cpu();
        have_cpu = true;
    }

    char line[ring::LINE_BYTES];
    LineBuilder builder{line, sizeof(line)};
    write_prefix(builder, time, have_time, cpu_id, have_cpu, level, domain);

    // Four bytes held back so the ellipsis and the newline always fit, even
    // for a body that overruns the rest of the line.
    auto reserve = builder.remaining();
    if (reserve > 4) {
        reserve = 4;
    }
    auto const body_end = builder.cursor() + (builder.remaining() - reserve);
    auto const body = ustl::fmt::vformat_to(BoundedOutput{builder.cursor(), body_end}, fmt, args);

    auto cursor = body.cursor();
    if (body.dropped()) {
        for (char c: {'.', '.', '.'}) {
            *cursor++ = c;
        }
    }
    *cursor++ = '\n';
    auto const total = static_cast<usize>(cursor - line);

    ring::append(line, total, time, cpu_id, level, domain);

    if (logger.is_null()) {
        // Nothing installed yet: an Error line still deserves the boot
        // port (early panic); everything else stays silent. The prefix is
        // part of `line`, so unlike the old routing the emergency output
        // carries it too.
        if (level == Level::Error) {
            emergency_write(line, total);
        }
        return;
    }
    logger.write(line, total);
}

template auto vlog(Logger &, Level, ustl::views::StringView, ustl::fmt::FormatArgs const &args)
    -> void;

auto vlog_dispatch(Logger &logger, Level level, ustl::views::StringView fmt,
                   ustl::fmt::FormatArgs const &args) -> void {
    vlog(logger, level, fmt, args);
}

namespace {
    /// Both dump_recent() overloads: pick the sink, then let `dump` replay
    /// onto it.
    template <typename Dump>
    auto dump_to_current(Dump const &dump) -> void {
        auto &logger = get_global_logger();
        if (!logger.is_null()) {
            dump(logger);
            return;
        }

        class EmergencySink final: public Logger {
          public:
            auto write(char const *s, usize n) -> void override {
                emergency_write(s, n);
            }
        };
        EmergencySink sink;
        dump(sink);
    }
} // namespace

auto dump_recent() -> void {
    dump_to_current([](Logger &sink) { static_cast<void>(ring::dump_to(sink)); });
}

auto dump_recent(Domain const &only) -> void {
    dump_to_current([&](Logger &sink) { static_cast<void>(ring::dump_to(sink, only)); });
}
} // namespace logz4
