/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// A shell that runs in ring 3 and talks to the kernel only through the vDSO.
///
/// What it is for: userboot proved that the kernel can build a process, map an
/// ELF into it and answer one system call. This is the next question -- can a
/// *program*, one that is not part of the kernel's own bring-up, do something
/// useful with that. Listing the kshell command registry is the smallest
/// useful thing, and it is useful precisely because the registry lives on the
/// other side of the privilege boundary: the names come out of the kernel's
/// linker sections, and nothing in this file knows any of them.
///
/// Every system call it makes goes through //system/lib/ou, which is the one
/// place that knows what the raw ABI looks like. This file names what it wants
/// (ou::debug_line, ou::command_list) and knows nothing else -- not the
/// per-call size limit, not the symbols the loader binds, not that a trap is
/// involved at all.
///
/// There is still no C or C++ runtime here: no libc, no crt1.o, no standard
/// library (see //system:user.config). The kernel jumps straight to the ELF
/// entry point with a stack and nothing else, so `_start` is the whole
/// runtime.

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include <ou/channel.hpp>
#include <ou/command.hpp>
#include <ou/debug.hpp>
#include <ou/handle.hpp>
#include <ou/object.hpp>
#include <ou/process.hpp>
#include <ou/str.hpp>

namespace {
/// The kshell command registry, fetched into this program's own memory.
///
/// Sized rather than grown: there is no allocator in this image. NoSpace comes
/// back when the registry outgrows it, and the part that fits is still shown
/// -- a truncated list beats no list when the point is to see what exists.
class Registry {
  public:
    auto fetch() -> ours::Status {
        return ou::command_list(m_text);
    }

    [[nodiscard]]
    auto text() const -> ou::Str {
        return ou::command_text(m_text);
    }

  private:
    char m_text[2048] = {};
};

constexpr ou::Str BANNER{"ours shell: the kernel's command registry, seen from ring 3"};

/// Prints "<what><status name>" on a line of its own.
auto report(ou::Str const what, ours::Status const status) -> void {
    static_cast<void>(ou::debug_line({what, ou::Str::from_c_str(ours::to_string(status), 32)}));
}

/// Gives up the handle this program was started with, and checks that the
/// kernel then refuses it.
///
/// The first close has to succeed: the value came from whoever started this
/// program, which put a handle into this process's table and its value into
/// %rdi -- the root job when the kernel started it, init's end of a channel
/// when init did. The second has to fail with BadHandle, because the same
/// number no longer names anything here -- and that is the handle table's
/// whole job, observed from the side of the boundary that relies on it.
///
/// Closing the channel end is harmless to init, although init wakes on
/// PeerClosed as well as Readable: answer_bootstrap() runs first, so "pong"
/// is already queued on init's end, and a message stays readable after the
/// writer's end is gone. Whichever signal wakes init, Readable is set by
/// then, and that is the one it checks.
auto check_handle_close(ours::HandleValue const bootstrap) -> void {
    report("shell: close(bootstrap) = ", ou::handle_close(bootstrap));
    report("shell: close again = ", ou::handle_close(bootstrap));
}

/// Prints "<what><got>" and whether that was |want|; answers whether it was.
auto expect(ou::Str const what, ours::Status const got, ours::Status const want) -> bool {
    auto const got_name = ou::Str::from_c_str(ours::to_string(got), 32);
    if (got == want) {
        static_cast<void>(ou::debug_line({what, got_name}));
        return true;
    }
    auto const want_name = ou::Str::from_c_str(ours::to_string(want), 32);
    static_cast<void>(ou::debug_line({what, got_name, " (expected ", want_name, ")"}));
    return false;
}

/// Whether the first |length| bytes of |bytes| spell |text|. By hand: there
/// is no libc in this image to call memcmp from.
auto spells(char const *const bytes, u32 const length, ou::Str const text) -> bool {
    if (length != text.size()) {
        return false;
    }
    for (u32 i = 0; i < length; ++i) {
        if (bytes[i] != text.data()[i]) {
            return false;
        }
    }
    return true;
}

/// Answers whoever started this program, if it was started with a channel.
///
/// Started by init, the bootstrap handle is one end of a channel with "ping"
/// on it (or about to be): read it, print it, answer "pong" -- the other half
/// of the first slice, whose log lines are the completion criterion
/// (design 5). Started by the kernel directly, the handle is the root job,
/// and a channel read on it answers BadType: there is nobody to answer, and
/// the checks below run as before.
///
/// Answers whether the handle was a channel -- false means this program was
/// started by the kernel and holds the root job.
auto answer_bootstrap(ours::HandleValue const bootstrap) -> bool {
    using ours::Signals;
    using ours::Status;

    char buffer[32] = {};
    u32 actual = 0;
    auto status = ou::channel_read(bootstrap, buffer, sizeof(buffer), &actual);
    if (status == Status::BadType) {
        static_cast<void>(ou::debug_line("shell: started by the kernel; no one to answer"));
        return false;
    }
    if (status == Status::ShouldWait) {
        auto observed = Signals::None;
        status = ou::object_wait_one(bootstrap, Signals::Readable | Signals::PeerClosed,
                                     ours::INSTANT_INFINITE, &observed);
        if (status == Status::Ok) {
            status = ou::channel_read(bootstrap, buffer, sizeof(buffer), &actual);
        }
    }
    if (status != Status::Ok) {
        report("shell: reading the bootstrap channel = ", status);
        return true;
    }

    static_cast<void>(ou::debug_line({"shell: bootstrap says \"", ou::Str(buffer, actual), "\""}));

    constexpr ou::Str PONG{"pong"};
    status = ou::channel_write(bootstrap, PONG.data(), static_cast<u32>(PONG.size()));
    if (status != Status::Ok) {
        report("shell: answering = ", status);
    }
    return true;
}

/// |value| in decimal, into |buffer|. By hand: no libc here.
auto decimal(u32 value, char (&buffer)[10]) -> ou::Str {
    auto at = sizeof(buffer);
    do {
        buffer[--at] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return {buffer + at, sizeof(buffer) - at};
}

/// Programs that fault, and the kernel still here afterwards (P1-A).
///
/// Each probe is a fresh fault_probe process (lib/syscall's test,
/// kernel/lib/syscall/test/fault_probe), told over a channel which fault to
/// commit. The kernel's answer to a fault from ring 3 is to kill the
/// process: its handle table goes with it, its end of the channel closes, and
/// this end sees PeerClosed with nothing to read. A probe that survives writes
/// back instead, and that is a failure. Between the two, the kernel's log
/// carries the report for each kill and the destruction of the process and
/// thread objects -- the parts of the chain this program cannot see.
///
/// Run only by the shell the kernel starts directly, which holds the root
/// job a new process needs; the dev image starts the shell from init, with a
/// channel instead, and does not package fault_probe.
auto run_probes(ours::HandleValue const job) -> void {
    using ours::Signals;
    using ours::Status;

    constexpr ou::Str PROBES[] = {
        "pf_unmapped", "pf_kernel", "ud2", "gp_hlt", "gp_int80", "gp_int3", "de", "db_tf", "gp_io",
    };

    u32 killed = 0;
    u32 survived = 0;
    for (auto const probe: PROBES) {
        ours::HandleValue here = ours::HANDLE_INVALID;
        ours::HandleValue there = ours::HANDLE_INVALID;
        if (!expect("shell: probe channel_create = ", ou::channel_create(&here, &there),
                    Status::Ok)) {
            return;
        }
        static_cast<void>(ou::channel_write(here, probe.data(), static_cast<u32>(probe.size())));

        // |there| leaves this process whatever happens.
        if (auto const status = ou::process_spawn_boot(job, "fault_probe", there);
            status != Status::Ok) {
            report("shell: spawning fault_probe = ", status);
            static_cast<void>(ou::handle_close(here));
            return;
        }

        auto observed = Signals::None;
        static_cast<void>(ou::object_wait_one(here, Signals::Readable | Signals::PeerClosed,
                                              ours::INSTANT_INFINITE, &observed));
        if (!!(observed & Signals::Readable)) {
            char answer[32] = {};
            u32 actual = 0;
            static_cast<void>(ou::channel_read(here, answer, sizeof(answer), &actual));
            static_cast<void>(ou::debug_line(
                {"shell: probe ", probe, " SURVIVED (", ou::Str(answer, actual), ")"}));
            ++survived;
        } else {
            static_cast<void>(ou::debug_line({"shell: probe ", probe, " killed"}));
            ++killed;
        }
        static_cast<void>(ou::handle_close(here));
    }

    char killed_text[10];
    char survived_text[10];
    static_cast<void>(
        ou::debug_line({"shell: probes done (", decimal(killed, killed_text), " killed, ",
                        decimal(survived, survived_text), " survived)"}));
}

/// A channel with both ends in this one process: everything the kernel
/// promises about one, checked from the side of the boundary that relies on
/// it. Nothing here blocks -- the waits are satisfied before they start (the
/// wait is level-triggered) -- so a single-threaded program can run it all.
/// A wait that really sleeps needs a second program to wake it.
auto check_channel_loopback() -> void {
    using ours::Signals;
    using ours::Status;

    auto ok = true;
    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (!expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        return;
    }

    constexpr ou::Str PING{"ping"};
    ok &= expect("shell: write(ping) = ",
                 ou::channel_write(a, PING.data(), static_cast<u32>(PING.size())), Status::Ok);

    // Already readable, so a poll must succeed and report Readable.
    auto observed = Signals::None;
    ok &= expect("shell: wait_one(READABLE, poll) = ",
                 ou::object_wait_one(b, Signals::Readable, 0, &observed), Status::Ok);
    ok &= !!(observed & Signals::Readable) && !!(observed & Signals::Writable);

    // Too small a buffer leaves the message where it is and says how long it
    // is.
    char buffer[16] = {};
    u32 actual = 0;
    ok &= expect("shell: read(2 bytes) = ", ou::channel_read(b, buffer, 2, &actual),
                 Status::BufferTooSmall);
    ok &= actual == PING.size();

    ok &=
        expect("shell: read = ", ou::channel_read(b, buffer, sizeof(buffer), &actual), Status::Ok);
    if (spells(buffer, actual, PING)) {
        static_cast<void>(ou::debug_line("shell: loopback ping ok"));
    } else {
        ok = false;
        static_cast<void>(ou::debug_line("shell: loopback read the wrong bytes"));
    }

    ok &= expect("shell: read on empty = ", ou::channel_read(b, buffer, sizeof(buffer), &actual),
                 Status::ShouldWait);

    // Closing one end tells the other: PeerClosed goes up, Writable comes
    // down, and every further read or write says so.
    ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);
    ok &= expect("shell: wait_one(PEER_CLOSED) = ",
                 ou::object_wait_one(b, Signals::PeerClosed, ours::INSTANT_INFINITE, &observed),
                 Status::Ok);
    ok &= !!(observed & Signals::PeerClosed) && !(observed & Signals::Writable);
    ok &= expect("shell: read after peer closed = ",
                 ou::channel_read(b, buffer, sizeof(buffer), &actual), Status::PeerClosed);
    ok &= expect("shell: write after peer closed = ",
                 ou::channel_write(b, PING.data(), static_cast<u32>(PING.size())),
                 Status::PeerClosed);
    ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: channel checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: channel checks FAILED"));
    }
}

/// Bad user pointers handed to the kernel: each call must come back InvArgs,
/// and the kernel must still be here to answer the next one.
///
/// The vDSO entry points are called directly rather than through ou, so that
/// the pointers reach the kernel exactly as written here. Two kinds:
///
///   UNMAPPED  inside the user half of the address space, so the range check
///             passes, but nothing is mapped there: the copy faults, the fault
///             handler cannot resolve it, and the copy's data-fault resume
///             turns it into InvArgs. Used as a source (copy_from_user) and as
///             a destination (copy_to_user), which are different copies.
///   KERNEL    the kernel half: refused by the range check before any copy.
///             It answered InvArgs before the resume existed and is here so
///             that it keeps doing so.
///
/// A third kind belongs here and is missing: a destination that is mapped
/// but read-only, such as the vDSO. Every x86 leaf PTE is currently built
/// writable (docs/repairs/260924/x86-kernel.md), so such a write succeeds;
/// the checks go in with that fix.
auto check_bad_pointers() -> void {
    using ours::Signals;
    using ours::Status;

    constexpr auto UNMAPPED = ours::VirtAddr{0x7fff'ffff'e000};
    constexpr auto KERNEL = ours::VirtAddr{0xffff'8000'0000'0000};

    auto ok = true;
    ok &= expect("shell: debug_write(unmapped) = ",
                 ours_debug_write(reinterpret_cast<char const *>(UNMAPPED), 8), Status::InvArgs);
    ok &= expect("shell: debug_write(kernel half) = ",
                 ours_debug_write(reinterpret_cast<char const *>(KERNEL), 8), Status::InvArgs);

    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (!expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        return;
    }

    // The message is copied in before the channel is touched, so a bad source
    // leaves the channel as it was.
    ok &= expect("shell: channel_write(from unmapped) = ",
                 ours_channel_write(a, ours::ChannelWriteOptions::None,
                                    reinterpret_cast<void const *>(UNMAPPED), 8, nullptr, 0),
                 Status::InvArgs);
    constexpr char BYTE = 'x';
    ok &= expect("shell: channel_write(1 byte) = ",
                 ours_channel_write(a, ours::ChannelWriteOptions::None, &BYTE, 1, nullptr, 0),
                 Status::Ok);

    // Read into nothing: the message is taken off the queue before it is
    // copied out, so when the copy fails the message is gone -- the same
    // choice Zircon makes, and the next read finds the channel empty.
    u32 actual_bytes = 0;
    u32 actual_handles = 0;
    ok &= expect("shell: channel_read(into unmapped) = ",
                 ours_channel_read(b, ours::ChannelReadOptions::None,
                                   reinterpret_cast<void *>(UNMAPPED), nullptr, 1, 0,
                                   &actual_bytes, &actual_handles),
                 Status::InvArgs);
    char buffer[4] = {};
    ok &= expect("shell: channel_read(after the lost message) = ",
                 ours_channel_read(b, ours::ChannelReadOptions::None, buffer, nullptr,
                                   sizeof(buffer), 0, &actual_bytes, &actual_handles),
                 Status::ShouldWait);

    // A poll, so that the wait ends at once and the kernel goes on to write
    // |observed| -- into nothing.
    ok &= expect("shell: wait_one(observed = unmapped, poll) = ",
                 ours_object_wait_one(b, Signals::Readable, 0,
                                      reinterpret_cast<Signals *>(UNMAPPED)),
                 Status::InvArgs);

    // Both handle values are written out before either handle is installed,
    // so the failed create leaves nothing behind and the next one works.
    ours::HandleValue c = ours::HANDLE_INVALID;
    ours::HandleValue d = ours::HANDLE_INVALID;
    ok &= expect("shell: channel_create(out0 = unmapped) = ",
                 ours_channel_create(ours::ChannelCreateOptions::None,
                                     reinterpret_cast<ours::HandleValue *>(UNMAPPED), &d),
                 Status::InvArgs);
    ok &= expect("shell: channel_create(after that) = ",
                 ours_channel_create(ours::ChannelCreateOptions::None, &c, &d), Status::Ok);

    ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);
    ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);
    ok &= expect("shell: close(c) = ", ou::handle_close(c), Status::Ok);
    ok &= expect("shell: close(d) = ", ou::handle_close(d), Status::Ok);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: bad-pointer checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: bad-pointer checks FAILED"));
    }
}
} // namespace

/// `extern "C"`, because ld.lld resolves the default entry point by the symbol
/// name `_start` and a mangled name would not answer to it.
///
/// |bootstrap| is the one handle this process starts with (%rdi), and
/// |vdso_base| where the vDSO was mapped (%rsi) -- see userabi's
/// userboot_init(). Finding the system calls is not this program's problem:
/// the calls below are undefined symbols that userboot binds to the vDSO's
/// definitions while it loads this image (elfload::Binder).
extern "C" [[noreturn]]
auto _start(ours::HandleValue const bootstrap, [[maybe_unused]] ours::VirtAddr const vdso_base)
    -> void {
    static_cast<void>(ou::debug_line(BANNER));
    // The probes need the root job, which is what |bootstrap| is when the
    // kernel started this program -- and check_handle_close() is about to
    // close it.
    if (!answer_bootstrap(bootstrap)) {
        run_probes(bootstrap);
    }
    check_handle_close(bootstrap);
    check_channel_loopback();
    check_bad_pointers();

    static Registry registry;
    auto const status = registry.fetch();

    static_cast<void>(ou::debug_line("--- commands ---"));
    static_cast<void>(ou::debug_write(registry.text()));
    if (status == ours::Status::NoSpace) {
        static_cast<void>(
            ou::debug_line("(truncated: the registry outgrew this program's buffer)"));
    } else if (status != ours::Status::Ok) {
        static_cast<void>(ou::debug_line("(command_list failed)"));
    }
    static_cast<void>(ou::debug_line("--- end ---"));

    // Nothing to do next, and nothing to exit to: there is no exit system call
    // yet and returning from _start would return into whatever the kernel left
    // on the stack. Halting here is the honest end of the program.
    //
    // `pause` is load-bearing, not decoration. C++'s forward-progress rule
    // makes a loop with no side effects undefined behaviour, and clang acts on
    // that licence: at -O2 it deletes `for (;;) {}` outright and lets control
    // fall off the end of the function. An `asm volatile` is a side effect the
    // optimiser must keep.
    for (;;) {
        __asm__ volatile("pause");
    }
}
