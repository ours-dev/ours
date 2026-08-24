#include <logz4/log.hpp>

#include <ustl/fmt/format_to.hpp>

#include <arch/x86/io.hpp>

namespace logz4 {
namespace {
    auto write(char const *s, int n) -> void {
        auto const port_ = 0x3F8;
        auto const is_transmit_empty = [&]() { return arch::inb(port_ + 5) & 0x20; };

        for (auto i = 0; i < n; ++i) {
            while (!is_transmit_empty())
            {}
            arch::outb(port_, s[i]);
        }
    }

    template <int N>
    auto write(char const (&s)[N]) -> void {
        return write(s, N);
    }
} // namespace anonymous

    template <typename FormatString>
    auto vlog(Logger &logger, Level level, FormatString fmt, ustl::fmt::FormatArgs const &args) -> void {
        // logger.log({ 
        //     .level = level,
        //     .args = args,
        //     .fmtstr = fmt,
        // });

        switch (level) {
            case Level::Status:
                write("[status]: ");
                break;
            case Level::Warn:
                write("[warn]: ");
                break;
            case Level::Info:
                write("[info]: ");
                break;
            case Level::Debug:
                write("[debug]: ");
                break;
            case Level::Trace:
                write("[trace]: ");
                break;
            case Level::Error:
                write("[error]: ");
                break;
        }

        static char s_format_buffer[512];

        auto end = ustl::fmt::vformat_to(s_format_buffer, fmt, args);
        *end = '\n';
        write(s_format_buffer, end - s_format_buffer + 1);
    }

    template auto vlog(Logger &, Level, ustl::views::StringView, ustl::fmt::FormatArgs const &args) -> void;
    // template auto vlog(Logger &, Level, ustl::views::WStringView, ustl::fmt::FormatArgs const &args) -> void;

} // namespace logz4