#include <ours/panic.hpp>

#include <arch/auto-interrupt-disabler.hpp>

#include <logz4/log.hpp>
#include <arch/halt.hpp>

namespace ours {
    template <typename String, typename FormatArgs>
    [[noreturn]]
    auto vpanic(String fmt, FormatArgs const &args) -> void {
        arch::AutoInterruptDisabler local_irq_disabler{};

        log::vlog(logz4::get_global_logger(), logz4::Level::Error, fmt, args);
        while (1) {
            arch::suspend();
        }
    }

    template auto vpanic(std::string_view, std::format_args const &) -> void;
    template auto vpanic(std::wstring_view, std::wformat_args const &) -> void;

} // namespace ours