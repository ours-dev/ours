#include <ours/panic.hpp>
#include <ours/phys/print.hpp>

namespace ours {
    [[noreturn]]
    auto panic() -> void {
        phys::vprint("panic");
        while (1);
    }

    template <typename StringView>
    [[noreturn]]
    auto vpanic(StringView fmt, ustl::fmt::FormatArgs const &args) -> void {
        phys::vprint("panic", args);
        while (1);
    }

    template auto vpanic(ustl::views::StringView fmt, ustl::fmt::FormatArgs const &args) -> void;
} // namespace ours