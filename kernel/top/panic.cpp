#include <ours/boot/options.hpp>
#include <ours/cpu.hpp>
#include <ours/ipi_service.hpp>
#include <ours/panic.hpp>
#include <ours/platform/power.hpp>

#include <atomic>

#include "crash_record.hpp"
#include <arch/auto_interrupt_disabler.hpp>
#include <arch/halt.hpp>
#include <logz4/log.hpp>
#include <logz4/ring.hpp>

namespace ours {
namespace {
    /// Set by the first CPU to panic, which is the one that prints the report.
    ///
    /// Relaxed. The read-modify-writes of one atomic form a single total
    /// order whatever memory order each uses, so exactly one exchange() ever
    /// reads false. Nothing else is published through the flag: what a later
    /// CPU leaves for the report reaches the first one through the Online
    /// mask (see stop_this_cpu()).
    std::atomic<bool> PANIC_STARTED{false};

    /// True for the first panic only: the exchange on `PANIC_STARTED` elects
    /// the one CPU that reports.
    auto claim_report() -> bool {
        return !PANIC_STARTED.exchange(true, std::memory_order_relaxed);
    }

    /// A sink that drops every line, for a line that belongs in the record
    /// ring and nowhere else.
    class RingOnly final: public logz4::Logger {
      public:
        auto write(char const *, usize) -> void override {}
    };

    /// Where every panic but the first ends: this CPU stops the way a
    /// suspended one does (x86_handle_ipi_suspend()).
    ///
    /// The Online bit goes first. This CPU has interrupts off and will never
    /// take the first CPU's suspend IPI, so without the bit
    /// IpiService::suspend_others() would wait out its whole bound for it.
    /// Clearing it is also a release, and suspend_others() reads the mask
    /// with an acquire: whatever this CPU put in the ring before it is there
    /// when the first CPU replays the ring.
    [[noreturn]]
    auto stop_this_cpu() -> void {
        Cpu::set_state(Cpu::id(), Cpu::State::Online, false);
        while (1) {
            arch::halt();
        }
    }

    /// Stops the rest of the machine before this CPU says anything.
    ///
    /// Disabling interrupts silences this CPU; it does nothing about the
    /// others, and on four CPUs that matters a great deal. A panic used to be
    /// followed by tens of thousands of lines from the CPUs still running,
    /// with the message and the ring dump somewhere in the middle of them --
    /// the dump itself interleaved line by line with live output from three
    /// other cores. Reading a panic became an exercise in grep.
    ///
    /// So the others are stopped before anything is printed. Best effort,
    /// and deliberately so: see IpiService::suspend_others().
    auto quiesce_machine() -> void {
        IpiService::suspend_others();
    }

    /// The line a panic report starts with, fixed so that a tool reading the
    /// serial log can tell a panic from any other error line;
    /// scripts/kshell_run.py matches it.
    ///
    /// The leading newline ends whatever line the console was in the middle
    /// of -- the kernel shell writes its prompt and its echo a byte at a time,
    /// outside the logger -- so the banner always starts a line of its own.
    constexpr auto BANNER = std::string_view("\nOURS KERNEL PANIC\n");

    /// Writes a line of the report's own -- the banner, the replay's markers
    /// -- to the sink directly rather than logging it. A log line also goes
    /// into the record ring, and the replay would then print it a second
    /// time: the banner at the replay's end, where a tool counting panics
    /// would count two. The routing is the one every logz4 line takes: the
    /// installed logger, or the emergency port before there is one.
    auto print_raw(std::string_view text) -> void {
        auto &logger = logz4::get_global_logger();
        if (logger.is_null()) {
            logz4::emergency_write(text.data(), text.size());
            return;
        }
        logger.write(text.data(), text.size());
    }

    auto print_banner() -> void {
        print_raw(BANNER);
    }

    /// How many of the ring's newest lines a panic replays once a logger is
    /// installed. Spelled out again in TAIL_MARKER, which says so to the
    /// reader.
    constexpr auto TAIL_LINES = usize{32};

    constexpr auto TAIL_MARKER = std::string_view(
        "--- recent log: the last 32 lines, most of them shown above already ---\n");
    constexpr auto FULL_MARKER =
        std::string_view("--- recent log: every line kept, from before any console ---\n");
    constexpr auto END_MARKER = std::string_view("--- end of recent log ---\n");
    constexpr auto REBOOT_MARKER =
        std::string_view("--- rebooting: kernel.panic.reboot is set ---\n");

    /// `kernel.panic.reboot`, read without the panic boot::options() raises
    /// before the options are parsed: a panic that early halts, as every
    /// panic did before the option existed.
    auto reboot_requested() -> bool {
        auto const *const options = boot::try_options();
        return options != nullptr && options->panic_reboot();
    }

    /// What led up to the panic: the record ring, replayed between two
    /// marker lines, so that a reader can tell where the live log stops and
    /// the replay begins.
    ///
    /// Once a logger is installed, every line that reaches the ring is also
    /// written to the console as it happens, so replaying the whole ring
    /// printed up to 256 lines the reader had just seen. Only a short tail
    /// is replayed then: the lines right before the panic, which are the
    /// ones worth having next to the message. They are also where a line
    /// that a stopped CPU put in the ring but never printed would be: its
    /// write still waiting on the serial lock, or a later panic that only
    /// records itself (see stop_this_cpu()). Replaying nothing at all would
    /// lose those lines; the tail is the middle ground that keeps them.
    ///
    /// Before any logger is installed, nothing in the ring reached a console
    /// except Error lines, and the ring is the only record there is. That is
    /// the case the ring exists for at panic time, so it is replayed whole.
    auto print_context() -> void {
        auto &logger = logz4::get_global_logger();
        if (logger.is_null()) {
            print_raw(FULL_MARKER);
            logz4::dump_recent();
        } else {
            print_raw(TAIL_MARKER);
            logz4::ring::dump_tail_to(logger, TAIL_LINES);
        }
        print_raw(END_MARKER);
    }
} // namespace

/// Renders the message the way panic() does, into a bounded stack buffer,
/// and reports it through the overload below, which is the one body: two
/// copies of the sequence below had to be kept in step by hand.
template <typename String, typename FormatArgs>
[[noreturn]]
auto vpanic(String fmt, FormatArgs const &args) -> void {
    char buffer[512];
    auto sink = PanicSink{buffer, buffer + sizeof(buffer)};
    std::vformat_to(PanicSink::Iterator{&sink}, fmt, args);
    vpanic(std::string_view(buffer, static_cast<usize>(sink.m_out - buffer)));
}

template auto vpanic(std::string_view, std::format_args const &) -> void;

/// The report is the banner, then the message, then the context.
///
/// The message used to come last, after a replay of the whole record ring,
/// so that the context read top to bottom like a log with the reason at the
/// bottom. But a panic report is read for its reason, and the replay is up
/// to 256 lines of polled serial output -- 2 s at 100 bytes a line and up to
/// 12 s at full lines, at 115200 baud -- any of which can stall. Whatever is
/// printed first is what survives a stall, so the reason goes first.
///
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
/// The message is logged, not written raw like the banner: that gives it the
/// time and CPU prefix, and the level tag that boot checks look for. It also
/// puts it in the ring, so the replay ends with it once more.
///
/// One report per machine, whoever panics. A CPU that panics while another
/// CPU's panic is already under way prints nothing: without that, four CPUs
/// panicking together printed four whole reports one after another, banner,
/// message and replay each, as each in turn waited out the others in
/// quiesce_machine(). It puts its message in the record ring instead, where
/// the first CPU's replay shows it in time order, and stops. That includes a
/// panic inside the panic path on the same CPU, which would otherwise start
/// the report over from the top.
///
/// Panic mode comes first, before anything prints, the other CPUs' last
/// words in quiesce_machine() included: the serial logger's lock may be held
/// by a CPU that will never let go, and in panic mode a write stops waiting
/// for it after a bound (logz4::enter_panic_mode()).
///
/// After the console report, the same message and log tail go into the crash
/// record (record_panic()), and with `kernel.panic.reboot` the machine resets
/// so that the next boot can read it. The record comes after the console:
/// the console is what someone may be watching now, and a fault while
/// writing the record must not cost the report.
[[noreturn]]
auto vpanic(std::string_view message) -> void {
    arch::AutoInterruptDisabler local_irq_disabler{};
    if (!claim_report()) {
        RingOnly ring_only;
        log::vlog(ring_only, logz4::Level::Error,
                  std::string_view("panicked while another panic was under way: {}"),
                  std::make_format_args(message));
        stop_this_cpu();
    }
    logz4::enter_panic_mode();
    quiesce_machine();

    print_banner();
    log::vlog(logz4::get_global_logger(), logz4::Level::Error, std::string_view("{}"),
              std::make_format_args(message));
    print_context();

    record_panic(message, TAIL_LINES);
    if (reboot_requested()) {
        print_raw(REBOOT_MARKER);
        platform_reboot();
    }
    while (1) {
        arch::halt();
    }
}

} // namespace ours
