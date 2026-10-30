/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The dump: every CPU's state, printed once by one CPU, then a panic.
///
/// Objects:
///  - REPORTER: the CPU that prints the report, none until one claims it.
///    One report per boot.
///  - REQUESTED: the CPUs the reporter has sent an NMI to, and waits for.
///  - SLOTS: one per CPU, the state a CPU leaves for the report. Each is
///    written only by its own CPU, once, and read by the reporter only after
///    the CPU has published it.
///
/// Events:
///  1. A CPU claims REPORTER, for one of two causes: its heartbeat's check found
///     another CPU stuck (checker.cpp), or it took an NMI that is not the
///     reporter's request -- one from outside the machine, through LINT1, or
///     the one `lockup nmi` sends its own CPU.
///  2. The reporter fills its own slot from the frame its heartbeat or its NMI
///     interrupted, stores every other online CPU into REQUESTED, and sends
///     each of them an NMI.
///  3. A target finds its bit in REQUESTED. It fills its slot, leaves the
///     Online mask, publishes the slot, and halts inside the NMI -- unless it
///     is in a panic of its own, which it goes back to once its slot is in.
///  4. The reporter waits for the targets' slots, for ANSWER_WAIT at
///     most, puts the logger in panic mode, prints one block per CPU -- a
///     stuck CPU's last -- and panics.
///
/// The targets print nothing. The reporter is the one writer, and it writes
/// only once the others are parked, so the report comes out whole and in
/// order rather than interleaved line by line with four CPUs' output.
#include "report.hpp"

#include <lockup_detector/lockup_detector.hpp>

#include <array>
#include <atomic>
#include <format>
#include <string_view>

#include "checker.hpp"
#include <arch/halt.hpp>
#include <arch/spin_hint.hpp>
#include <arch/tick.hpp>
#include <logz4/log.hpp>
#include <ours/arch_mp.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/panic.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/syscall/time.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/ticks_per_second.hpp>
#include <sampler/frame_pointers.hpp>
#include <sampler/stack_window.hpp>
#include <ustl/util/enum_bits.hpp>

namespace lockup_detector {
using ours::CpuMask;
using ours::CpuNum;

namespace {
    /// How many addresses a CPU's backtrace keeps, its own pc the first.
    constexpr auto MAX_FRAMES = usize{16};

    /// A thread's name as Thread keeps it (ktl::Name<32>), terminator
    /// included.
    constexpr auto NAME_BYTES = usize{32};

    /// The longest backtrace line: sixteen " +0x" offsets of up to eight
    /// digits each, with room to spare.
    constexpr auto BACKTRACE_BYTES = usize{256};

    /// The longest line of checks without a beat: " cpu<N> <count>" for
    /// every CPU, at most 20 characters each.
    constexpr auto COUNTS_BYTES = usize{20} * ours::MAX_CPU;

    /// How long the reporter waits for the targets. A CPU that is going to
    /// answer an NMI answers within microseconds; the bound is for one that
    /// never will, and it is paid once.
    ///
    /// A time, not spins, because a spin's cost differs a hundredfold between
    /// TCG and hardware (logz4's PANIC_LOCK_WAIT says the same). And a time,
    /// not a count of arch::Tick: the counter runs at 1 GHz under QEMU's x86,
    /// at a few GHz on a real one and at 10 MHz on QEMU's RISC-V board, so
    /// the 2 000 000 000 counts this used to be were 2 s on the first, 0.4 s
    /// on a 5 GHz one and 200 s on the last.
    constexpr auto ANSWER_WAIT = ours::Milliseconds(2'000);

    /// The counts of arch::Tick that answer_wait_ticks() waits while
    /// ours::time::ticks_in() cannot turn ANSWER_WAIT into counts, because
    /// ours::time::ticks_per_second() does not know the rate. That is the
    /// case of a PC whose local APIC timer calibration failed, which still
    /// runs every CPU, and of a report made before the calibration has run,
    /// when only the boot CPU runs and there is no target to wait for.
    /// 2 000 000 000 counts is the count this used to be, written for a
    /// 1 GHz counter, so such a PC waits exactly as long as it did: the
    /// whole ANSWER_WAIT under QEMU's x86.
    constexpr auto ANSWER_WAIT_FALLBACK_COUNT = u64{2'000'000'000};

    /// ANSWER_WAIT in arch::Tick counts.
    auto answer_wait_ticks() -> u64 {
        auto ticks = ours::time::ticks_in(ANSWER_WAIT);
        if (ticks == 0) {
            ticks = ANSWER_WAIT_FALLBACK_COUNT;
        }
        return ticks;
    }

    /// Reads one word of the interrupted stack for the walk, refusing
    /// anything outside |context|'s window: the walk follows frame pointers
    /// it cannot trust, and a fault cannot be taken inside an NMI.
    auto read_stack(void *context, u64 const address, u64 *const out) -> bool {
        auto const &window = *static_cast<sampler::StackWindow const *>(context);
        if (!window.contains(address)) {
            return false;
        }
        *out = *reinterpret_cast<u64 const *>(address);
        return true;
    }

    /// Where the kernel image starts: printed addresses are offsets from it,
    /// which is what `llvm-objdump -d vmours` shows, since the image is
    /// linked at 0 and relocated at load.
    auto image_start() -> u64 {
        return static_cast<u64>(ours::mm::get_kernel_virt_base());
    }

    /// The state a Slot is in. At namespace scope rather than inside the
    /// class, because the class's own opt-in to the bit operators would be a
    /// friend that only an unevaluated call names, which a class with
    /// internal linkage reports as never emitted.
    enum class SlotFlags : u32 {
        None,
        /// capture() is done and the reporter may read the slot.
        Ready = 0_bit,
        /// The CPU was in a panic of its own, and went back to it rather
        /// than parking.
        Panicking = 1_bit,
    };
    USTL_ENABLE_ENUM_BITMASK(SlotFlags);

    /// What one CPU leaves for the report: the context its NMI interrupted.
    ///
    /// Written by its own CPU only, in its NMI, once; read by the reporter
    /// only after is_ready(). The Ready bit is the slot's one atomic, and the
    /// release that sets it publishes every plain field capture() wrote.
    class Slot final {
      public:
        /// Fills the slot from |frame|. Reads only this CPU's own state, and
        /// its own thread's kernel stack through a bounded reader, so it
        /// takes no lock and cannot fault.
        auto capture(arch::IrqFrame const &frame) -> void;

        /// Says the slot is whole. Release: the reporter's acquire in
        /// is_ready() then sees everything capture() wrote.
        auto publish() -> void {
            m_flags.store(Flags::Ready, std::memory_order_release);
        }

        /// publish(), for a CPU that goes back to a panic of its own. One
        /// store sets both bits, so the reporter never sees Ready alone.
        auto publish_panicking() -> void {
            m_flags.store(Flags::Ready | Flags::Panicking, std::memory_order_release);
        }

        [[nodiscard]]
        auto is_ready() const -> bool {
            return !!(m_flags.load(std::memory_order_acquire) & Flags::Ready);
        }

        /// Whether the CPU went back to a panic of its own. Asked only after
        /// is_ready(), whose acquire already saw the one store that sets it.
        [[nodiscard]]
        auto is_panicking() const -> bool {
            return !!(m_flags.load(std::memory_order_relaxed) & Flags::Panicking);
        }

        /// Logs the slot as CPU |cpu|'s block of the report. The reporter's
        /// only, in panic mode.
        auto print(CpuNum cpu, bool reporter) const -> void;

      private:
        using Flags = SlotFlags;

        arch::IrqFrame m_frame{};
        std::array<char, NAME_BYTES> m_thread{};
        usize m_spinlocks{0};
        /// The beats this CPU had had when it filled the slot, its
        /// heartbeat. Every CPU counts from the checker's arming, one a
        /// second, and a beat that came late counts once however many
        /// seconds it covered, so even on a healthy machine the counts can
        /// differ by one or two; a CPU that stopped beating falls behind by
        /// one a second on top of that. A dump asked for from outside, when
        /// no CPU beats and no checker has counts to print, has only these
        /// to say in what order the CPUs went quiet.
        u64 m_heartbeat{0};
        std::array<u64, MAX_FRAMES> m_frames{};
        usize m_depth{0};
        std::atomic<Flags> m_flags{Flags::None};
    };

    auto Slot::capture(arch::IrqFrame const &frame) -> void {
        m_frame = frame;
        m_spinlocks = ours::sync::SpinLock::num_held_cpu();
        m_heartbeat = priv::this_cpu_heartbeat();
        m_frames[0] = frame.pc();
        m_depth = 1;

        // Null only before the boot CPU has a thread at all.
        auto *const thread = ours::task::Thread::Current::get();
        if (thread == nullptr) {
            return;
        }
        auto const *const name = thread->name();
        auto length = usize{0};
        while (length + 1 < m_thread.size() && name[length] != '\0') {
            m_thread[length] = name[length];
            ++length;
        }
        m_thread[length] = '\0';

        // A user stack is not walked: it need not be mapped, and a fault
        // cannot be taken here. A kernel stack is walked only within the
        // running thread's own stack, from the interrupted sp to its top;
        // code interrupted on any other stack keeps its pc alone.
        if (frame.from_user()) {
            return;
        }
        auto const &stack = thread->kernel_stack();
        auto window = sampler::StackWindow::on_stack(frame.sp(), stack.base(), stack.top());
        if (window.empty()) {
            return;
        }
        auto const reader = unwind::MemoryReader{&window, &read_stack};
        m_depth = sampler::walk_frame_pointers(frame.pc(), frame.fp(), reader, m_frames.data(),
                                               MAX_FRAMES);
    }

    auto Slot::print(CpuNum const cpu, bool const reporter) const -> void {
        auto role = std::string_view();
        if (reporter) {
            role = " (reporting)";
        } else if (is_panicking()) {
            role = " (in a panic of its own, reported after this)";
        }
        auto const thread = std::string_view(m_thread.data());
        auto interrupts = std::string_view("off");
        if (m_frame.irqs_enabled()) {
            interrupts = "on";
        }

        if (m_frame.from_user()) {
            log::error("lockup: cpu{}{} thread {} in user space at {:#x}, interrupts {}, "
                       "{} spin locks held, {} beats",
                       cpu, role, thread, m_frame.pc(), interrupts, m_spinlocks, m_heartbeat);
            arch_dump_registers(m_frame);
            return;
        }
        log::error("lockup: cpu{}{} thread {} at +{:#x}, interrupts {}, {} spin locks held, {} "
                   "beats",
                   cpu, role, thread, m_frame.pc() - image_start(), interrupts, m_spinlocks,
                   m_heartbeat);
        arch_dump_registers(m_frame);

        // One line for the whole walk, so that four CPUs' blocks fit the
        // tail the crash record keeps.
        auto text = std::array<char, BACKTRACE_BYTES>{};
        auto *out = text.data();
        auto *const end = text.data() + text.size();
        for (auto i = usize{0}; i < m_depth; ++i) {
            auto const room = static_cast<std::ptrdiff_t>(end - out);
            out = std::format_to_n(out, room, " +{:#x}", m_frames[i] - image_start()).out;
        }
        log::error("lockup: cpu{} backtrace{}", cpu,
                   std::string_view(text.data(), static_cast<usize>(out - text.data())));
    }

    /// One slot per CPU, indexed by CPU number. A plain array rather than
    /// CPU_LOCAL data: the reporter finds every slot by number, and a slot
    /// needs no per-CPU unit, so an NMI that comes before CpuLocal::init()
    /// still has one.
    constinit std::array<Slot, ours::MAX_CPU> SLOTS{};

    /// The CPU printing the report. Taken once, by compare-exchange, and
    /// never given back: one report per boot, since the report ends in a
    /// panic. Relaxed: the claim elects a CPU and publishes nothing.
    constinit std::atomic<CpuNum> REPORTER{ours::INVALID_CPU_NUM};

    /// The CPUs the reporter has sent an NMI to. Stored once, before the
    /// first NMI is sent; read by every NMI. It only says who is asked: what
    /// the reporter needs back from a target comes through the target's
    /// slot, so a relaxed load of it is enough.
    constinit std::atomic<CpuMask> REQUESTED{};

    /// Where every CPU but the reporter ends. NMIs stay blocked, since only
    /// an iretq would unblock them and this never returns, and interrupts are
    /// off, as the NMI gate left them: the CPU stays halted until INIT or a
    /// reset. Nothing here gives a state back, so there is no scope to
    /// leave.
    [[noreturn]]
    auto park() -> void {
        for (;;) {
            arch::halt();
        }
    }

    /// A target's answer: its state into its slot, then out of the way.
    ///
    /// The Online bit goes before the slot is published. The reporter
    /// panics once every slot is in, and the panic stops the CPUs still
    /// online with an IPI this CPU can no longer take; out of the mask
    /// first, it is not waited for. Clearing the bit is a release, and the
    /// publish after it another, so a reporter that sees the slot also sees
    /// the bit gone.
    [[noreturn]]
    auto answer(arch::IrqFrame const &frame, CpuNum const self) -> void {
        auto &slot = SLOTS[self];
        slot.capture(frame);
        ours::Cpu::set_state(self, ours::Cpu::State::Online, false);
        slot.publish();
        park();
    }

    /// A target that is in a panic of its own: its state into its slot, and
    /// back to that panic.
    ///
    /// Parked, it would take its panic's report with it: the reporter's own
    /// panic, after the dump, finds the panic claimed, puts its message in
    /// the record ring only and stops, so the banner, this CPU's message,
    /// the crash record and the reset never come. A panic says more than the
    /// dump, so this CPU stays Online and returns to it. Its panic waits,
    /// within its bound, for the reporter to leave the Online mask, which
    /// the reporter does once the dump is out.
    ///
    /// The slot is filled once: the iretq back to the panic lets a later NMI
    /// in, and by then the reporter may be reading the slot.
    auto answer_from_panic(arch::IrqFrame const &frame, CpuNum const self) -> void {
        auto &slot = SLOTS[self];
        if (slot.is_ready()) {
            return;
        }
        slot.capture(frame);
        slot.publish_panicking();
    }

    /// Whether every CPU in |targets| has published its slot.
    auto all_answered(CpuMask const &targets) -> bool {
        auto answered = true;
        targets.for_each([&answered](CpuNum const cpu) {
            if (!SLOTS[cpu].is_ready()) {
                answered = false;
            }
        });
        return answered;
    }

    /// Spins until every CPU in |targets| has answered, or until
    /// ANSWER_WAIT has passed. The ones that have not are reported as such.
    auto wait_for_answers(CpuMask const &targets) -> void {
        auto const bound = answer_wait_ticks();
        auto const start = arch::Tick::get().count;
        while (!all_answered(targets) && arch::Tick::get().count - start < bound) {
            arch::spin_hint();
        }
    }

    /// Ends whatever line the console was left in the middle of, written
    /// straight to the sink and not logged, so it stays out of the record
    /// ring. A CPU parked by its NMI may have stopped half way through a
    /// line, with the console lock still held; the first line of the report
    /// is written without the lock once the panic-mode bound runs out, and
    /// would otherwise be glued to the end of that half line.
    auto end_partial_line() -> void {
        auto &logger = logz4::get_global_logger();
        if (logger.is_null()) {
            logz4::emergency_write("\n", 1);
            return;
        }
        logger.write("\n", 1);
    }

    /// Whom the reporter asked for their state, and which of them it could
    /// not send an NMI to.
    struct Asked {
        CpuMask targets;
        CpuMask unsent;
    };

    /// The reporter's part up to the printing: events 2 and 4 of the file
    /// comment, short of panic mode. Returns once every CPU asked has
    /// answered, or the wait has run out.
    ///
    /// REQUESTED is stored with seq_cst for the fence that comes with it: on
    /// x86 the store is an xchg, which drains this CPU's store buffer, so the
    /// mask is visible everywhere before the ICR write that sends the first
    /// NMI -- and a target's NMI reads it after that.
    auto gather(arch::IrqFrame const &frame, CpuNum const self) -> Asked {
        SLOTS[self].capture(frame);
        SLOTS[self].publish();

        auto targets = ours::Cpu::online_mask();
        targets.set(self, false);
        REQUESTED.store(targets, std::memory_order_seq_cst);
        auto const unsent = ours::arch_mp_send_nmi(targets);
        auto sent = targets;
        sent &= ~unsent;
        wait_for_answers(sent);
        return Asked{.targets = targets, .unsent = unsent};
    }

    /// CPU |cpu|'s part of the report: its block, or why it has none.
    auto print_cpu(CpuNum const cpu, CpuNum const self, Asked const &asked) -> void {
        if (SLOTS[cpu].is_ready()) {
            SLOTS[cpu].print(cpu, cpu == self);
        } else if (asked.unsent.test(cpu)) {
            log::error("lockup: cpu{} was sent no NMI: this CPU's ICR never went idle", cpu);
        } else if (asked.targets.test(cpu)) {
            log::error("lockup: cpu{} did not answer its NMI", cpu);
        }
    }

    /// Every CPU's part, in CPU order but for |last|, which comes after the
    /// rest: the crash record keeps only the report's last lines, and the
    /// CPU a report is about is the block it must keep. INVALID_CPU_NUM
    /// keeps CPU order throughout.
    ///
    /// Every CPU's slot is looked at, not only the requested ones: a CPU
    /// that took an NMI from outside just as the claim was taken answers
    /// without having been asked.
    auto print_cpus(CpuNum const self, Asked const &asked, CpuNum const last) -> void {
        for (auto cpu = CpuNum{0}; cpu < ours::MAX_CPU; ++cpu) {
            if (cpu != last) {
                print_cpu(cpu, self, asked);
            }
        }
        if (last < ours::MAX_CPU) {
            print_cpu(last, self, asked);
        }
    }

    /// The reporter's notes on the CPUs it asked, as one line: how many
    /// checks in a row each went without a beat. A CPU that went quiet
    /// earlier has the larger count, so the line says in what order they
    /// stopped.
    auto print_counts(CpuNum const self, CpuMask const &targets, priv::Notes const &notes) -> void {
        auto text = std::array<char, COUNTS_BYTES>{};
        auto *out = text.data();
        auto *const end = text.data() + text.size();
        targets.for_each([&out, end, &notes](CpuNum const cpu) {
            auto const room = static_cast<std::ptrdiff_t>(end - out);
            out = std::format_to_n(out, room, " cpu{} {}", cpu, notes[cpu].unchanged()).out;
        });
        log::error("lockup: checks in a row without a beat, as cpu{} counted them:{}", self,
                   std::string_view(text.data(), static_cast<usize>(out - text.data())));
    }

    /// The report an NMI asked for, from the reporter's own NMI.
    [[noreturn]]
    auto report_nmi(arch::IrqFrame const &frame, CpuNum const self) -> void {
        auto const asked = gather(frame, self);

        logz4::enter_panic_mode();
        end_partial_line();
        log::error("lockup: dump asked for by an NMI on cpu{}; each CPU's state follows", self);
        print_cpus(self, asked, ours::INVALID_CPU_NUM);
        ours::panic("lockup: dump asked for by an NMI on cpu{}", self);
    }
} // namespace

namespace priv {
    auto report_claimed() -> bool {
        return REPORTER.load(std::memory_order_relaxed) != ours::INVALID_CPU_NUM;
    }

    auto report_stuck(arch::IrqFrame const &frame, CpuNum const self, CpuNum const stuck,
                      Notes const &notes) -> void {
        auto reporter = ours::INVALID_CPU_NUM;
        if (!REPORTER.compare_exchange_strong(reporter, self, std::memory_order_relaxed)) {
            return;
        }
        auto const asked = gather(frame, self);

        // One check a second, so the count of checks is the seconds.
        auto const seconds = notes[stuck].unchanged();
        logz4::enter_panic_mode();
        end_partial_line();
        log::error(
            "lockup: cpu{} had no heartbeat for {} s, found by cpu{}; each CPU's state follows",
            stuck, seconds, self);
        print_counts(self, asked.targets, notes);
        print_cpus(self, asked, stuck);
        ours::panic("lockup: cpu{} had no heartbeat for {} s", stuck, seconds);
    }
} // namespace priv

auto on_nmi(arch::IrqFrame const &frame) -> void {
    auto const self = ours::Cpu::id();
    if (REQUESTED.load(std::memory_order_relaxed).test(self)) {
        // The request crossed a panic this CPU started after the reporter
        // had looked for one: the panic's report goes on, with this CPU's
        // state in the dump ahead of it.
        if (ours::this_cpu_in_panic()) {
            answer_from_panic(frame, self);
            return;
        }
        answer(frame, self);
    }

    // A panic is already stopping the machine and printing its own report;
    // a second writer would interleave with it.
    if (ours::panic_in_progress()) {
        return;
    }

    auto reporter = ours::INVALID_CPU_NUM;
    if (REPORTER.compare_exchange_strong(reporter, self, std::memory_order_relaxed)) {
        report_nmi(frame, self);
    }

    // The reporter itself, interrupted by an NMI from outside while it
    // reports. When it reports from its heartbeat timer, the NMI has landed on top of
    // that report, which goes on once this returns. When it reports from an
    // NMI of its own, a second one cannot arrive until an iretq unblocks
    // NMIs, and nothing in the report returns from an exception: reaching
    // here means one did. This NMI then runs on the NMI stack the report is
    // still using, over the report's upper frames. Returning lets the report
    // carry on on that damaged stack, which may yet get it out; parking
    // would end it for certain.
    if (reporter == self) {
        return;
    }

    // Another CPU is reporting, and this NMI came from outside before that
    // CPU's request did: answer it all the same.
    answer(frame, self);
}
} // namespace lockup_detector
