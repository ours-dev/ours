#include <arch/auto_interrupt_disabler.hpp>
#include <arch/halt.hpp>
#include <logz4/log.hpp>
#include <ours/panic.hpp>

namespace ours {
template <typename String, typename FormatArgs>
[[noreturn]]
auto vpanic(String fmt, FormatArgs const &args) -> void {
    arch::AutoInterruptDisabler local_irq_disabler{};

    log::vlog(logz4::get_global_logger(), logz4::Level::Error, fmt, args);
    while (1) {
        arch::halt();
    }
}

template auto vpanic(std::string_view, std::format_args const &) -> void;

[[noreturn]]
auto vpanic(std::string_view fmt) -> void {
    arch::AutoInterruptDisabler local_irq_disabler{};

    log::vlog(logz4::get_global_logger(), logz4::Level::Error, fmt, std::make_format_args());
    while (1) {
        arch::halt();
    }
}

} // namespace ours
