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

    /// The line a panic report starts with, fixed so that a tool reading the
    /// serial log can tell a panic from any other error line. Zircon's
    /// "ZIRCON KERNEL PANIC" (DLog::BluescreenInit()) is the model, and its
    /// test harnesses match it the way scripts/kshell_run.py matches this.
    ///
    /// The leading newline ends whatever line the console was in the middle
    /// of -- the kernel shell writes its prompt and its echo a byte at a time,
    /// outside the logger -- so the banner always starts a line of its own.
    constexpr auto BANNER = std::string_view("\nOURS KERNEL PANIC\n");

    /// Written to the sink directly rather than logged. A log line also goes
    /// into the record ring, and the replay that follows the message would
    /// then print the banner a second time, at its end, where a tool counting
    /// panics would count two. The routing is the one every logz4 line takes:
    /// the installed logger, or the emergency port before there is one.
    auto print_banner() -> void {
        auto &logger = logz4::get_global_logger();
        if (logger.is_null()) {
            logz4::emergency_write(BANNER.data(), BANNER.size());
            return;
        }
        logger.write(BANNER.data(), BANNER.size());
    }

    /// What led up to the panic: the record ring, replayed.
    auto print_context() -> void {
        logz4::dump_recent();
    }
} // namespace

/// The report is the banner, then the message, then the context.
///
/// The message used to come last, after a replay of the whole record ring,
/// so that the context read top to bottom like a log with the reason at the
/// bottom. But a panic report is read for its reason, and the replay is up
/// to 256 lines of polled serial output -- 2 s at 100 bytes a line and up to
/// 12 s at full lines, at 115200 baud -- any of which can stall. Whatever is
/// printed first is what survives a stall, so the reason goes first. Linux's
/// panic() prints "Kernel panic - not syncing: ..." before dump_stack() and
/// kmsg_dump(), and Zircon's platform_panic_start() prints its banner before
/// anything else, for the same reason.
///
/// The message is logged, not written raw like the banner: that gives it the
/// time and CPU prefix, and the level tag that boot checks look for. It also
/// puts it in the ring, so the replay ends with it once more.
///
/// Panic mode comes first, before anything prints, the other CPUs' last
/// words in quiesce_machine() included: the serial logger's lock may be held
/// by a CPU that will never let go, and in panic mode a write stops waiting
/// for it after a bound (logz4::enter_panic_mode()).
template <typename String, typename FormatArgs>
[[noreturn]]
auto vpanic(String fmt, FormatArgs const &args) -> void {
    arch::AutoInterruptDisabler local_irq_disabler{};
    logz4::enter_panic_mode();
    quiesce_machine();

    print_banner();
    log::vlog(logz4::get_global_logger(), logz4::Level::Error, fmt, args);
    print_context();
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
///
/// The report comes out in the same order as the overload above prints it.
[[noreturn]]
auto vpanic(std::string_view message) -> void {
    arch::AutoInterruptDisabler local_irq_disabler{};
    logz4::enter_panic_mode();
    quiesce_machine();

    print_banner();
    log::vlog(logz4::get_global_logger(), logz4::Level::Error, std::string_view("{}"),
              std::make_format_args(message));
    print_context();
    while (1) {
        arch::halt();
    }
}

} // namespace ours
