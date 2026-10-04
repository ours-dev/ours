/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `port.test [ROUNDS]`: a port's observers against the port's tear-down
// (docs/design/260924/userspace-filesystem.md section 7.4). dbg image only:
// it is the one with a kernel shell, and with the assertions this relies on.
//
// The shell checks every answer a program can see from a port. What it
// cannot do is race: it has one thread. This races the two things that can
// end a registered PortObserver at the same moment:
//   - the port losing its last handle: on_zero_handles() claims each armed
//     observer under the port lock, then takes it off its object;
//   - the watched object ending the wait: on_match() (a signal) or
//     on_cancel() (its handle closing), under the object's lock, which
//     queues the packet or frees the observer -- or, finding the tear-down
//     already claimed it, leaves it alone.
//
// Each round arms RACED waits on one channel end and STRANDED waits on the
// other. A thread on one CPU closes the port while a thread on another ends
// the raced waits, after a delay that differs from round to round: by
// raising Readable (even rounds), or by closing the end as a handle close
// would, cancelling its waits and then dropping what may be its last
// reference (odd rounds), so that the tear-down's own reference to the
// object is what keeps it alive. Nothing ever ends the stranded waits but
// the tear-down. Whatever the interleaving:
//   - the port is gone after every round, which a weak reference checks in
//     every build; the stranded waits alone would keep it alive without a
//     tear-down;
//   - no observer is freed twice or left behind: dbg asserts on the list
//     hooks, on the counts in ~PortDispatcher, and on the empty observer
//     list in ~Dispatcher as each channel end goes;
//   - the tear-down freed at least STRANDED observers a round, and some
//     callback found its observer already claimed, so both orders ran
//     (PortDispatcher::tear_down_counts()).
//
// First, once: HandleTable::attach_port_observer() turned away at
// PORT_MAX_OBSERVERS frees the observer it made, or the port could never be
// destroyed. A table of its own, driven to the limit and past it, then the
// port's handle closed: the port must be gone.
//
// A console command rather than a host test for the reason mutex.stress
// gives: the race is between CPUs.
#include <ours/cpu.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/channel_dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>
#include <string_view>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using object::ChannelDispatcher;
    using object::Dispatcher;
    using object::Handle;
    using object::HandleTable;
    using object::KernelHandle;
    using object::PortDispatcher;
    using object::PortObserver;
    using task::Thread;

    constexpr usize DEFAULT_ROUNDS = 200;
    constexpr usize RACED = 64;
    constexpr usize STRANDED = 8;

    /// One round's objects, shared with its two threads. On the heap, and
    /// freed only after both threads have been joined.
    ///
    /// The port is declared last so that it is destroyed first: closing it
    /// tears down whatever is still armed, which must happen before a channel
    /// end with observers on it goes.
    struct Round {
        KernelHandle<ChannelDispatcher> stranded_end;
        KernelHandle<ChannelDispatcher> raced_end;
        bool cancel = false;
        usize delay = 0;
        std::atomic<u32> ready{0};
        std::atomic<bool> go{false};
        KernelHandle<PortDispatcher> port;
    };

    /// The stand-in handle every raced wait is armed through: what cancel()
    /// is given when that handle closes. The stranded waits use another.
    auto raced_handle(Round &round) -> void const * {
        return &round.raced_end;
    }

    auto stranded_handle(Round &round) -> void const * {
        return &round.stranded_end;
    }

    /// Both threads arrive, then start together.
    auto start_together(Round &round) -> void {
        round.ready.fetch_add(1, std::memory_order_acq_rel);
        while (!round.go.load(std::memory_order_acquire)) {
            arch::spin_hint();
        }
    }

    /// Closes the port: its last handle, so on_zero_handles() runs here.
    auto closer_routine(void *const arg) -> i32 {
        auto &round = *static_cast<Round *>(arg);
        start_together(round);
        round.port.reset(ustl::Arc<PortDispatcher>());
        return 0;
    }

    /// Ends the raced waits from the object's side.
    auto ender_routine(void *const arg) -> i32 {
        auto &round = *static_cast<Round *>(arg);
        start_together(round);
        for (usize spin = 0; spin < round.delay; ++spin) {
            arch::spin_hint();
        }
        if (round.cancel) {
            // What a handle close does: cancel the waits made through it,
            // then let go of the object -- the last holder here, unless the
            // tear-down is holding it between its two steps.
            round.raced_end.dispatcher()->cancel(raced_handle(round));
            round.raced_end.reset(ustl::Arc<ChannelDispatcher>());
        } else {
            round.raced_end.dispatcher()->update_state(Signals::None, Signals::Readable);
        }
        return 0;
    }

    auto spawn_on(char const *const name, Thread::StartRoutine const routine, Round *const round,
                  CpuNum const cpu) -> Thread * {
        auto *const thread = Thread::spawn(name, routine, round, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        auto _ = thread->resume();
        return thread;
    }

    /// Arms |count| waits for Readable on |end|, through |handle|.
    auto arm(Round &round, KernelHandle<ChannelDispatcher> &end, void const *const handle,
             usize const count) -> bool {
        auto &port = *round.port.dispatcher().as_ptr_mut();
        auto &object = end.dispatcher();
        for (usize key = 0; key < count; ++key) {
            auto *const observer = new (mm::GAF_KERNEL)
                PortObserver(round.port.dispatcher(), object.as_ptr_mut(), key, Signals::Readable);
            if (observer == nullptr) {
                return false;
            }
            if (object->attach_port_observer(observer, port, handle, Signals::Readable) !=
                Status::Ok) {
                delete observer;
                return false;
            }
        }
        return true;
    }

    /// One round; answers whether every check held.
    auto run_round(usize const index, CpuNum const closer_cpu, CpuNum const ender_cpu) -> bool {
        auto *const round = new (mm::GAF_KERNEL) Round{};
        if (round == nullptr) {
            return false;
        }
        round->cancel = (index % 2) != 0;
        round->delay = (index * 37) % 256;

        Rights rights = Rights::None;
        if (PortDispatcher::create(&round->port, &rights) != Status::Ok ||
            ChannelDispatcher::create(&round->stranded_end, &round->raced_end, &rights) !=
                Status::Ok) {
            delete round;
            return false;
        }
        auto const weak = round->port.dispatcher().downgrade();

        if (!arm(*round, round->raced_end, raced_handle(*round), RACED) ||
            !arm(*round, round->stranded_end, stranded_handle(*round), STRANDED)) {
            log::error("port.test:   round {}: arming a wait failed", index);
            // No thread has the round yet. Freeing it closes the port first,
            // which tears down whatever was armed.
            delete round;
            return false;
        }

        auto *const closer = spawn_on("port-test-closer", closer_routine, round, closer_cpu);
        auto *const ender = spawn_on("port-test-ender", ender_routine, round, ender_cpu);
        if (closer == nullptr || ender == nullptr) {
            log::error("port.test:   round {}: spawning a thread failed", index);
            // The one that did start is waiting for |go|: let it run, wait
            // for it, and only then free what it uses.
            round->go.store(true, std::memory_order_release);
            for (auto *const thread: {closer, ender}) {
                i32 ignored = 0;
                if (thread != nullptr &&
                    thread->join(&ignored, time::Deadline::infinite()) != Status::Ok) {
                    log::error("port.test:   joining it failed; leaking the round");
                    return false;
                }
            }
            delete round;
            return false;
        }
        while (round->ready.load(std::memory_order_acquire) != 2) {
            arch::spin_hint();
        }
        round->go.store(true, std::memory_order_release);

        i32 retcode = 0;
        auto const joined = closer->join(&retcode, time::Deadline::infinite()) == Status::Ok &&
                            ender->join(&retcode, time::Deadline::infinite()) == Status::Ok;
        if (!joined) {
            log::error("port.test:   round {}: joining a thread failed; leaking the round", index);
            return false;
        }

        // Every observer is gone, the stranded ones by the tear-down alone.
        // One left registered would hold the port.
        auto const gone = weak.expired();
        if (!gone) {
            log::error("port.test:   round {}: the port outlived its last handle", index);
        }

        // The channel ends go here (the raced one already, in a cancel
        // round). In dbg, ~Dispatcher asserts that nothing is still linked on
        // them.
        delete round;
        return gone;
    }

    /// HandleTable::attach_port_observer() at the limit: the observer it made
    /// for a refused wait must be freed, or it would hold the port for ever.
    auto check_refusal() -> bool {
        auto table = ustl::Arc<HandleTable>::adopt(new (mm::GAF_KERNEL) HandleTable());
        if (!table) {
            return false;
        }
        KernelHandle<PortDispatcher> port;
        KernelHandle<ChannelDispatcher> end0;
        KernelHandle<ChannelDispatcher> end1;
        Rights port_rights = Rights::None;
        Rights end_rights = Rights::None;
        if (PortDispatcher::create(&port, &port_rights) != Status::Ok ||
            ChannelDispatcher::create(&end0, &end1, &end_rights) != Status::Ok) {
            return false;
        }
        auto const weak = port.dispatcher().downgrade();
        auto port_handle = Handle::make(KernelHandle<Dispatcher>(ustl::move(port)), port_rights);
        auto end_handle = Handle::make(KernelHandle<Dispatcher>(ustl::move(end1)), end_rights);
        if (!port_handle || !end_handle) {
            return false;
        }
        auto const port_value = table->add(ustl::move(port_handle));
        auto const end_value = table->add(ustl::move(end_handle));

        usize armed = 0;
        auto status = Status::Ok;
        while (armed <= PORT_MAX_OBSERVERS) {
            status = table->attach_port_observer(end_value, port_value, armed, Signals::Readable,
                                                 WaitAsyncOptions::None);
            if (status != Status::Ok) {
                break;
            }
            armed += 1;
        }
        auto ok = status == Status::NoResource && armed == PORT_MAX_OBSERVERS;
        for (usize more = 0; more < STRANDED; ++more) {
            ok &=
                table->attach_port_observer(end_value, port_value, armed + more, Signals::Readable,
                                            WaitAsyncOptions::None) == Status::NoResource;
        }

        // The port's handle closes: the tear-down takes its 1024 waits, and
        // a refused observer that was never freed would be all that holds it.
        {
            // Scoped: a placeholder `_` would keep the handle until the end of
            // the function, and the port with it.
            auto const closed = table->remove(port_value);
        }
        auto const gone = weak.expired();
        {
            auto const closed = table->remove(end_value);
        }

        log::info("port.test:   {} waits armed through a handle table before NoResource; the "
                  "port {} once its handle closed",
                  armed, gone ? "was destroyed" : "OUTLIVED its handle");
        return ok && gone;
    }

    auto parse_rounds(int const argc, char **const argv) -> usize {
        if (argc < 2) {
            return DEFAULT_ROUNDS;
        }
        usize rounds = 0;
        for (auto const c: std::string_view(argv[1])) {
            if (c < '0' || c > '9') {
                return 0;
            }
            rounds = rounds * 10 + static_cast<usize>(c - '0');
        }
        return rounds;
    }

    auto port_test_command(int argc, char **argv) -> int {
        auto const rounds = parse_rounds(argc, argv);
        if (rounds == 0) {
            log::error("port.test: usage: port.test [ROUNDS]");
            return 1;
        }

        auto failed = false;
        if (!check_refusal()) {
            log::error("port.test:   a wait refused at the limit was not freed");
            failed = true;
        }

        auto const num_cpus = Cpu::num_online_cpus();
        auto const closer_cpu = CpuNum{0};
        auto const ender_cpu = num_cpus > 1 ? CpuNum{1} : CpuNum{0};
        if (num_cpus < 2) {
            log::warn("port.test: one CPU online; the two threads take turns rather than race");
        }

        auto const before = PortDispatcher::tear_down_counts();
        auto const started = time::SteadyClock::now();
        usize bad = 0;
        for (usize index = 0; index < rounds && bad == 0; ++index) {
            if (!run_round(index, closer_cpu, ender_cpu)) {
                bad += 1;
            }
        }
        auto const elapsed = time::SteadyClock::now() - started;
        auto const after = PortDispatcher::tear_down_counts();
        auto const torn_down = static_cast<usize>(after.torn_down - before.torn_down);
        auto const yielded = static_cast<usize>(after.yielded - before.yielded);

        log::info("port.test:   {} round(s) x ({} raced + {} stranded) waits on CPUs {} and {} in "
                  "{} ms: {} torn down, {} callbacks yielded to the tear-down, {} bad round(s)",
                  rounds, RACED, STRANDED, closer_cpu, ender_cpu,
                  duration_cast<Milliseconds>(elapsed).count(), torn_down, yielded, bad);
        if (bad != 0) {
            log::error("port.test:   {} bad round(s)", bad);
            failed = true;
        }
        if (torn_down < rounds * STRANDED) {
            log::error("port.test:   the tear-down freed {} observers, fewer than the {} stranded",
                       torn_down, rounds * STRANDED);
            failed = true;
        }
        if (num_cpus > 1 && yielded == 0) {
            log::error("port.test:   no callback ever met the tear-down; the race was not "
                       "exercised");
            failed = true;
        }
        if (failed) {
            log::error("port.test: FAILED");
            return 1;
        }
        log::info("port.test: ok");
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PortTest, "port.test", ours::port_test_command,
             "port.test [ROUNDS]: race a closing port's tear-down against its observers");
