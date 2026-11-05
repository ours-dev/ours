/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `ioport.test`: the I/O bitmap behind ioports_request and ioports_release
// (.agents/tasks/microkernel-interrupts/PLAN.md, step 10), as the TSS a CPU
// consults on every in and out from ring 3 shows it.
//
//   - The runs: on a user address space of the test's own, opens and closes
//     that stand alone, touch, overlap, swallow several runs, trim runs and
//     split one leave open exactly the ports a bit-per-port model of the same
//     steps says, in a window around them; the last port there is opens and
//     closes like any other.
//   - The arguments: a range past the last port is InvArgs, and an empty one
//     changes nothing.
//   - The switch: back on the kernel's address space the test's ports are
//     closed in this CPU's TSS, on a second user address space with no ports
//     of its own too, and on the test's again they are open.
//   - The reload: while a second thread runs in the test's address space on
//     another CPU, a port that starts closed there is open in that CPU's TSS
//     by the time the open returns, and closed by the time the close returns,
//     while this CPU, on the kernel's address space, never opens it.
//   - The switch while a close is on its way: that CPU, holding the close's
//     reload off with interrupts, switches to another address space after
//     the runs changed; the switch closes the port the runs no longer name.
//
// A console command because what is checked is the bitmap the CPU consults,
// which only the kernel can read, and the reload needs two CPUs. Ring 0 is
// not asked about ports, so the test opens and closes them without touching
// a device. The shell's ioport check covers the system calls and a process
// using its own ports; this covers what the shell cannot see: that a close
// leaves nothing open, the reload of another CPU, and the window between a
// change and its reload.
#include <ours/arch/aspace_layout.hpp>
#include <ours/arch/x86/io_bitmap.hpp>
#include <ours/arch/x86/percpu.hpp>
#include <ours/cpu.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>
#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    /// The ports the run checks compare with the model: every port below
    /// this. The steps stay inside it, so a port opened by mistake on either
    /// side of a run is inside it too.
    constexpr auto WINDOW = u32{0x100};

    /// What should be open, port by port.
    using Model = std::array<bool, WINDOW>;

    /// A port the reload check opens on another CPU. Outside the two runs the
    /// runs check leaves open, so that it starts closed there: inside one,
    /// the open would already be seen before it is made.
    constexpr auto RELOAD_PORT = u32{0x80};

    /// Generous for the reason mutex.stress's STUCK_AFTER is: QEMU's TCG.
    constexpr auto LOST_AFTER = Seconds(5);

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("ioport.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("ioport.test: FAIL  {}", name);
    }

    /// Spins until |condition| holds or LOST_AFTER has passed.
    template <typename Condition>
    auto spin_until(Condition const &condition) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!condition()) {
            if (time::SteadyClock::now() >= give_up) {
                return false;
            }
            arch::spin_hint();
        }
        return true;
    }

    /// Puts the calling thread on |next|, loading its page tables and its
    /// ports on this CPU, and hands back what it was on before; as
    /// usercopy.test does, with preemption off so that the scheduler and the
    /// switch here agree on which address space is being left.
    auto attach(mm::VmAspace *const next) -> mm::VmAspace * {
        task::AutoPreemptDisabler no_preemption;
        auto *const thread = task::Thread::Current::get();
        auto *const previous = thread->aspace();
        thread->set_aspace(next);
        mm::VmAspace::switch_aspace(previous, next);
        return previous;
    }

    /// Whether the window's ports open in this CPU's TSS are those of
    /// |model|, logging the first that is not. One CPU's TSS, read with
    /// preemption off: a thread moved to another CPU midway would read half
    /// of each.
    auto window_is(Model const &model, char const *const after) -> bool {
        task::AutoPreemptDisabler no_preemption;
        for (u32 port = 0; port < WINDOW; ++port) {
            auto const open = IoBitmap::open_here(port);
            if (open != model[port]) {
                log::error("ioport.test:   after {}: port {:#x} is {}, expected {}", after, port,
                           open, model[port]);
                return false;
            }
        }
        return true;
    }

    auto user_aspace(char const *const name, ustl::Arc<mm::VmAspace> *const out) -> bool {
        auto const status =
            mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE, mm::VmasKind::User,
                                 mm::VmasFlags::None, name, out);
        if (status != Status::Ok) {
            log::error("ioport.test: could not create an address space: {}", to_string(status));
            return false;
        }
        return true;
    }

    struct Step {
        char const *name;
        bool open;
        u32 base;
        u32 end;
    };

    /// Each step's runs afterwards, worked through by hand, are in its name's
    /// comment; the check compares the TSS with the model, not with these.
    constexpr Step RUN_STEPS[] = {
        // [0x10, 0x18)
        {"open a run alone", true, 0x10, 0x18},
        // [0x10, 0x18) [0x20, 0x28)
        {"open a second, apart from it", true, 0x20, 0x28},
        // [0x10, 0x28)
        {"open the gap between two runs, touching both", true, 0x18, 0x20},
        // [0x10, 0x28) [0x30, 0x34) [0x38, 0x3c)
        {"open two short runs", true, 0x30, 0x34},
        {"open the second short run", true, 0x38, 0x3C},
        // [0x10, 0x28) [0x2c, 0x40)
        {"open a range over two runs and the gap between them", true, 0x2C, 0x40},
        // [0x10, 0x14) [0x16, 0x28) [0x2c, 0x40)
        {"close the middle of a run, splitting it", false, 0x14, 0x16},
        // [0x10, 0x14) [0x16, 0x24) [0x30, 0x40)
        {"close across the end of one run and the start of the next", false, 0x24, 0x30},
        // [0x10, 0x14) [0x16, 0x24) [0x30, 0x40) [0x43, 0x4d) [0x80, 0xc0)
        {"open a range inside two bytes", true, 0x43, 0x4D},
        {"open whole bytes", true, 0x80, 0xC0},
        // [0x10, 0x14) [0x16, 0x24) [0x30, 0x4d) [0x80, 0xc0)
        {"open a range overlapping a run's end and the next run's start", true, 0x3F, 0x44},
        // [0x10, 0x14) [0x16, 0x24) [0x30, 0x45) [0x46, 0x4d) [0x80, 0xc0)
        {"close one port inside a run", false, 0x45, 0x46},
        // [0x10, 0x14) [0x16, 0x24) [0x30, 0x45) [0x46, 0x4d) [0x80, 0xc0)
        {"close ports already closed", false, 0x50, 0x60},
        // [0x10, 0x14) [0x16, 0x24) [0x30, 0x45) [0x46, 0x4d) [0x80, 0xc0)
        {"open ports already open", true, 0x84, 0x88},
        // none
        {"close the whole window", false, 0x00, WINDOW},
        // [0x07, 0x09) [0x60, 0x68)
        {"open across a byte boundary", true, 0x07, 0x09},
        {"open a byte-aligned run", true, 0x60, 0x68},
    };

    /// The steps above on |aspace|, which the calling thread is on, each
    /// compared with the model; then the last port there is, and the
    /// arguments. Leaves the last two runs open, for the switch check.
    auto check_runs(Tally &tally, mm::VmAspace &aspace, Model &model) -> void {
        auto &arch_aspace = aspace.arch_aspace();
        auto failed_step = static_cast<char const *>(nullptr);
        for (auto const &step: RUN_STEPS) {
            auto status = Status::Ok;
            if (step.open) {
                status = arch_aspace.open_ioports(step.base, step.end - step.base);
            } else {
                status = arch_aspace.close_ioports(step.base, step.end - step.base);
            }
            for (auto port = step.base; port < step.end; ++port) {
                model[port] = step.open;
            }
            if (status != Status::Ok) {
                log::error("ioport.test:   {} answered {}", step.name, to_string(status));
                failed_step = step.name;
                break;
            }
            if (!window_is(model, step.name)) {
                failed_step = step.name;
                break;
            }
        }
        if (failed_step != nullptr) {
            log::error("ioport.test:   the runs went wrong at \"{}\"", failed_step);
        }
        verdict(tally, failed_step == nullptr,
                "opens and closes leave open what a bit per port says, and nothing beside it");

        constexpr auto LAST = X86Tss::NUM_IO_PORTS - 1;
        auto const opened = arch_aspace.open_ioports(LAST - 7, 8) == Status::Ok;
        auto open_at_top = false;
        auto closed_below = false;
        {
            task::AutoPreemptDisabler no_preemption;
            open_at_top = IoBitmap::open_here(LAST);
            closed_below = !IoBitmap::open_here(LAST - 8);
        }
        auto const reclosed = arch_aspace.close_ioports(LAST - 7, 8) == Status::Ok;
        auto closed_at_top = false;
        {
            task::AutoPreemptDisabler no_preemption;
            closed_at_top = !IoBitmap::open_here(LAST);
        }
        verdict(tally, opened && open_at_top && closed_below && reclosed && closed_at_top,
                "the last port opens and closes like any other");

        auto const past = arch_aspace.open_ioports(LAST, 2);
        auto const empty = arch_aspace.open_ioports(0x20, 0);
        verdict(tally, past == Status::InvArgs, "a range past the last port is InvArgs");
        verdict(tally, empty == Status::Ok && window_is(model, "an empty open"),
                "an empty range changes nothing");
    }

    /// The test's ports follow it from one address space to the next.
    auto check_switch(Tally &tally, mm::VmAspace &aspace, mm::VmAspace &kernel_aspace,
                      Model const &model) -> void {
        constexpr Model NONE{};

        attach(&kernel_aspace);
        verdict(tally, window_is(NONE, "switching to the kernel's address space"),
                "the kernel's address space opens none of them");

        ustl::Arc<mm::VmAspace> other;
        if (!user_aspace("ioport.test.other", &other)) {
            verdict(tally, false, "create a second address space");
            attach(&aspace);
            return;
        }
        attach(other.as_ptr_mut());
        auto const none_in_other = window_is(NONE, "switching to another address space");
        attach(&aspace);
        auto const open_again = window_is(model, "switching back");
        verdict(tally, none_in_other, "another address space opens none of them");
        verdict(tally, open_again, "switching back opens them again");

        attach(&kernel_aspace);
        if (auto const status = other->destroy(); status != Status::Ok) {
            log::error("ioport.test: could not destroy the second address space: {}",
                       to_string(status));
        }
        attach(&aspace);
    }

    /// What the observer on the other CPU and the command share.
    struct ReloadCase {
        mm::VmAspace *kernel_aspace;
        std::atomic<bool> stop{false};
        std::atomic<bool> done{false};
        /// Whether RELOAD_PORT was open, at the read |reads| counts.
        std::atomic<bool> open{false};
        std::atomic<usize> reads{0};
        /// The switch made while a close is on its way: the command asks for
        /// it (|hold|), the observer has interrupts off (|held|), and what the
        /// observer saw: its TSS stale before the switch, and RELOAD_PORT
        /// still open after it. Read by the command once |done| is set.
        std::atomic<bool> hold{false};
        std::atomic<bool> held{false};
        bool saw_stale = false;
        bool open_after_switch = true;
    };

    /// The window IoBitmap's version is there for, made on purpose: the
    /// observer's CPU switches to another address space after a close has
    /// changed the runs and before the close's reload reaches it. Interrupts
    /// are off from before the command closes until after the switch, so the
    /// reload's interrupt waits, and the close cannot return before then.
    ///
    /// The switch has to close RELOAD_PORT although the runs no longer name
    /// it. Closing the runs as they now stand, which is all a switch could do
    /// without the version, would leave it open for the next address space.
    /// Ends on the kernel's address space.
    auto switch_while_stale(ReloadCase &c) -> void {
        // Preemption off around the stretch with interrupts off, so that the
        // disabler attach() ends inside it finds no reschedule to make there.
        task::AutoPreemptDisabler const no_preemption;
        ::arch::AutoInterruptDisabler const no_interrupts;
        c.held.store(true, std::memory_order_release);
        c.saw_stale = spin_until([] { return IoBitmap::stale_here(); });
        attach(c.kernel_aspace);
        c.open_after_switch = IoBitmap::open_here(RELOAD_PORT);
    }

    /// Runs in the test's address space, set before it was resumed, on a CPU
    /// of its own, and reads RELOAD_PORT's bit in that CPU's TSS in a loop,
    /// until the command says stop, or asks for the switch while stale.
    auto observe_loop(void *const arg) -> i32 {
        auto &c = *static_cast<ReloadCase *>(arg);
        while (!c.stop.load(std::memory_order_acquire) && !c.hold.load(std::memory_order_acquire)) {
            auto open = false;
            {
                task::AutoPreemptDisabler no_preemption;
                open = IoBitmap::open_here(RELOAD_PORT);
            }
            c.open.store(open, std::memory_order_relaxed);
            c.reads.fetch_add(1, std::memory_order_release);
            arch::spin_hint();
        }
        // Off the test's address space before saying so: once the command
        // sees |done| it destroys it.
        if (c.hold.load(std::memory_order_acquire)) {
            switch_while_stale(c);
        } else {
            attach(c.kernel_aspace);
        }
        // Last touch of |c|: once the command sees this, it frees it.
        c.done.store(true, std::memory_order_release);
        return 0;
    }

    /// What the observer read in a read that began after the caller called
    /// this: two reads on from the count seen now, since the one counted next
    /// may have begun before.
    auto observed_after(ReloadCase const &c) -> ustl::Option<bool> {
        auto const now = c.reads.load(std::memory_order_acquire);
        if (!spin_until([&] { return c.reads.load(std::memory_order_acquire) >= now + 2; })) {
            return ustl::none();
        }
        return c.open.load(std::memory_order_relaxed);
    }

    /// The command runs on the kernel's address space here, so the only CPU
    /// running the test's is the observer's: the reload reaches it through
    /// an interrupt, not as the caller's own share of sync_exec().
    ///
    /// Answers whether no observer is left: one that did not stop may still
    /// run in |aspace|, which the caller must then not free.
    [[nodiscard]]
    auto check_reload(Tally &tally, mm::VmAspace &aspace, mm::VmAspace &kernel_aspace) -> bool {
        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 2) {
            log::warn("ioport.test:   skipped the reload check: it needs two online CPUs, have {}",
                      num_cpus);
            return true;
        }

        auto *const c = new (mm::ALLOC_KERNEL) ReloadCase{};
        if (c == nullptr) {
            verdict(tally, false, "allocate the reload case");
            return true;
        }
        c->kernel_aspace = &kernel_aspace;

        auto *const observer =
            task::Thread::spawn("ioport-test-observer", observe_loop, c, task::DEFAULT_PRIORITY);
        if (observer == nullptr) {
            verdict(tally, false, "spawn the observer");
            delete c;
            return true;
        }
        observer->set_aspace(&aspace);
        auto const cpu = static_cast<CpuNum>((Cpu::id() + 1) % num_cpus);
        observer->set_cpu_affinity_hard(CpuMask::of(cpu));
        // detach() and resume() separately: detach_and_resume() is declared
        // but has no definition yet.
        auto _ = observer->detach();
        auto _ = observer->resume();

        auto const running =
            spin_until([c] { return c->reads.load(std::memory_order_acquire) != 0; });
        verdict(tally, running, "the observer runs in the test's address space");

        auto &arch_aspace = aspace.arch_aspace();
        auto before = ustl::Option<bool>();
        auto opened = ustl::Option<bool>();
        auto closed = ustl::Option<bool>();
        auto open_here = true;
        if (running) {
            before = observed_after(*c);
        }
        if (running && arch_aspace.open_ioports(RELOAD_PORT, 1) == Status::Ok) {
            opened = observed_after(*c);
            {
                task::AutoPreemptDisabler no_preemption;
                open_here = IoBitmap::open_here(RELOAD_PORT);
            }
            if (arch_aspace.close_ioports(RELOAD_PORT, 1) == Status::Ok) {
                closed = observed_after(*c);
            }
        }

        // The switch while a close is on its way (switch_while_stale()):
        // RELOAD_PORT open on the observer's CPU again, then closed while that
        // CPU holds the close's reload off. The close returns only once that
        // CPU has run the reload, which it does after its switch.
        auto reopened = ustl::Option<bool>();
        auto closed_while_held = false;
        if (closed.is_some() && arch_aspace.open_ioports(RELOAD_PORT, 1) == Status::Ok) {
            reopened = observed_after(*c);
            c->hold.store(true, std::memory_order_release);
            if (spin_until([c] { return c->held.load(std::memory_order_acquire); })) {
                closed_while_held = arch_aspace.close_ioports(RELOAD_PORT, 1) == Status::Ok;
            }
        }

        c->stop.store(true, std::memory_order_release);
        if (!spin_until([c] { return c->done.load(std::memory_order_acquire); })) {
            log::error("ioport.test:   the observer did not stop; leaking its state and the test's "
                       "address space");
            verdict(tally, false, "the observer stops");
            return false;
        }
        auto const saw_stale = c->saw_stale;
        auto const open_after_switch = c->open_after_switch;
        delete c;

        verdict(tally, before.is_some() && !*before, "the port starts closed on the other CPU");
        verdict(tally, opened.is_some() && *opened,
                "a port opened here is open on the other CPU when the open returns");
        verdict(tally, closed.is_some() && !*closed, "and closed there when the close returns");
        verdict(tally, !open_here, "this CPU, on another address space, never opens it");
        verdict(tally, reopened.is_some() && *reopened && closed_while_held && saw_stale,
                "the other CPU switches away while a close's reload is on its way to it");
        verdict(tally, !open_after_switch,
                "and that switch closes the port, though the runs no longer name it");
        return true;
    }

    auto ioport_test_command(int, char **) -> int {
        Tally tally;

        ustl::Arc<mm::VmAspace> aspace;
        if (!user_aspace("ioport.test", &aspace)) {
            return 1;
        }
        auto *const previous = attach(aspace.as_ptr_mut());

        Model model{};
        check_runs(tally, *aspace, model);
        check_switch(tally, *aspace, *previous, model);

        // On the thread's own address space for the rest, and before the
        // test's goes, so that nothing is left running on tables being freed.
        attach(previous);
        if (!check_reload(tally, *aspace, *previous)) {
            // The observer may still run on these tables, so they are leaked
            // with its state rather than freed under it.
            auto _ = aspace.into_raw();
        } else if (auto const status = aspace->destroy(); status != Status::Ok) {
            log::error("ioport.test: could not destroy the address space: {}", to_string(status));
            tally.failed += 1;
        }

        if (tally.failed != 0) {
            log::error("ioport.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("ioport.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(IoportTest, "ioport.test", ours::ioport_test_command,
             "open and close I/O ports in an address space of the test's own and check the TSS");
