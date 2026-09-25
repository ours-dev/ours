#include <ours/ipi_service.hpp>
#include <ours/panic.hpp>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/halt.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    /// Stops the rest of the machine before this CPU says anything.
    ///
    /// Disabling interrupts silences this CPU; it does nothing about the
    /// others, and on four CPUs that matters a great deal. A panic used to be
    /// followed by tens of thousands of lines from the CPUs still running,
    /// with the message and the ring dump somewhere in the middle of them --
    /// the dump itself interleaved line by line with live output from three
    /// other cores. Reading a panic became an exercise in grep.
    ///
    /// Linux's panic() calls smp_send_stop() before its own printing for
    /// exactly this reason. Best effort, and deliberately so: see
    /// IpiService::suspend_others().
    auto quiesce_machine() -> void {
        IpiService::suspend_others();
    }
} // namespace

template <typename String, typename FormatArgs>
[[noreturn]]
auto vpanic(String fmt, FormatArgs const &args) -> void {
    arch::AutoInterruptDisabler local_irq_disabler{};
    quiesce_machine();

    logz4::dump_recent();
    log::vlog(logz4::get_global_logger(), logz4::Level::Error, fmt, args);
    while (1) {
        arch::halt();
    }
}

template auto vpanic(std::string_view, std::format_args const &) -> void;

/// The message arrives already rendered -- panic() and panic_assertion() both
/// format into a bounded stack buffer before they get here -- so it is passed
/// to the logger as an *argument* of a "{}" format string rather than as the
/// format string itself. Handing it over as the format string, which is what
/// this used to do, meant re-parsing text that had just been produced by the
/// formatter: any brace the message legitimately contained (an assertion
/// message quoting a format string, a symbol with braces in it) was read as a
/// replacement field with no argument behind it, and the panic died inside
/// std::format instead of printing.
[[noreturn]]
auto vpanic(std::string_view message) -> void {
    arch::AutoInterruptDisabler local_irq_disabler{};
    quiesce_machine();

    logz4::dump_recent();
    log::vlog(logz4::get_global_logger(), logz4::Level::Error, std::string_view("{}"),
              std::make_format_args(message));
    while (1) {
        arch::halt();
    }
}

} // namespace ours
