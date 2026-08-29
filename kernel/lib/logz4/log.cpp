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
        // N counts the terminating NUL; sending it to the serial port would
        // emit a stray zero byte.
        return write(s, N - 1);
    }

    /// A bounded output iterator for std::vformat_to.
    ///
    /// vformat_to writes as much as the format string demands, and a bare
    /// `char *` tells it nothing about where the buffer ends -- one long log
    /// line would run straight off the end of s_format_buffer below. Anything
    /// past the limit is dropped instead.
    class BoundedOutput {
      public:
        using difference_type = std::ptrdiff_t;

        BoundedOutput() = default;

        BoundedOutput(char *first, char *last)
            : cursor_(first), last_(last) {}

        auto operator*() -> BoundedOutput & { return *this; }
        auto operator++() -> BoundedOutput & { return *this; }
        auto operator++(int) -> BoundedOutput { return *this; }

        auto operator=(char c) -> BoundedOutput & {
            if (cursor_ < last_) {
                *cursor_++ = c;
            }
            return *this;
        }

        [[nodiscard]] auto cursor() const -> char * { return cursor_; }

      private:
        char *cursor_ = nullptr;
        char *last_ = nullptr;
    };
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

        // One byte held back so the newline below always has somewhere to go.
        auto const out = ustl::fmt::vformat_to(
            BoundedOutput{s_format_buffer, s_format_buffer + sizeof(s_format_buffer) - 1}, fmt,
            args);

        auto *const end = out.cursor();
        *end = '\n';
        write(s_format_buffer, static_cast<int>(end - s_format_buffer + 1));
    }

    template auto vlog(Logger &, Level, ustl::views::StringView, ustl::fmt::FormatArgs const &args) -> void;
    // template auto vlog(Logger &, Level, ustl::views::WStringView, ustl::fmt::FormatArgs const &args) -> void;

} // namespace logz4