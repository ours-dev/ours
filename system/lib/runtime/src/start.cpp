/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// What runs between the entry point (crt0.S) and main, and after main.
#include <runtime/process_args.hpp>
#include <runtime/startup.hpp>
#include <runtime/startup_handles.hpp>

#include <ou/debug.hpp>
#include <ou/str.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

/// crt0.S: a tail jump to main, which C++ may not call itself.
extern "C" auto runtime_call_main(int argc, char **argv) -> int;

extern "C" [[noreturn]]
auto runtime_start(ours::HandleValue bootstrap, ours::VirtAddr vdso_base) -> void;

namespace runtime {
namespace {
    // Everything the process was started with, for as long as it runs:
    // argv's strings and every string process_args() hands out point into
    // |message|, so it cannot live in a frame that returns.
    //
    // Static and constant-initialised (constinit), because nothing runs a
    // program's dynamic initialisers: there is no .init_array walk, here or
    // in the loader. A global that needed a constructor would silently start
    // out as zeros.
    //
    // A channel message is at most 4 KiB and 64 handles, so fixed buffers
    // hold any bootstrap message, and there is no need for Zircon's size
    // query before the read.
    alignas(ours::ProcArgs) constinit u8 message[ours::CHANNEL_MAX_MSG_BYTES] = {};
    constinit ours::HandleValue handles[ours::CHANNEL_MAX_MSG_HANDLES] = {};
    constinit u32 infos[ours::CHANNEL_MAX_MSG_HANDLES] = {};
    constinit ProcessArgs startup_args;
    constinit StartupHandles startup_handles;
    constinit ours::VirtAddr vdso = 0;

    [[noreturn]]
    auto fail(ou::Str const what, ours::Status const status) -> void {
        static_cast<void>(
            ou::debug_line({"runtime: ", what, ou::Str::from_c_str(ours::to_string(status), 32)}));
        ours_process_exit(EXIT_START_FAILED);
    }

    /// Reads the processargs message off |bootstrap| into the buffers above,
    /// checks it, and closes |bootstrap|.
    ///
    /// Waits for the message first, as Zircon's newer start-up code does
    /// (ld::ProcessargsBuffer, which says why: the parent "might have started
    /// this process before sending its bootstrap message"). Every starter
    /// here does exactly that: the message carries the child's root VMA, which
    /// only exists once the child does, so the kernel, userboot, init and the
    /// shell all write it after the spawn -- and a child that read before the
    /// write landed, on another CPU, would find nothing. The wait ends on READABLE, or on PEER_CLOSED
    /// if the starter drops its end without writing, and then the read
    /// reports PeerClosed rather than hanging; only a starter that keeps its
    /// end and never writes leaves this waiting, which is that starter's bug.
    ///
    /// Then one read, which must find the message: a message that does not
    /// fit is bigger than any channel message can be.
    ///
    /// The channel is closed once read, whatever came of it, as Zircon's
    /// start-up code closes it: the message was its only purpose, and a
    /// program that needs to talk to its starter is handed a channel for
    /// that among its startup handles. Its peer sees PEER_CLOSED as soon as
    /// the runtime has read the message, before main runs.
    auto read_processargs(ours::HandleValue const bootstrap) -> void {
        auto observed = ours::Signals::None;
        static_cast<void>(ours_object_wait_one(bootstrap,
                                               ours::Signals::Readable | ours::Signals::PeerClosed,
                                               ours::INSTANT_INFINITE, &observed));

        u32 num_bytes = 0;
        u32 num_handles = 0;
        auto status = ours_channel_read(bootstrap, ours::ChannelReadOptions::None, message, handles,
                                        sizeof(message), ours::CHANNEL_MAX_MSG_HANDLES, &num_bytes,
                                        &num_handles);
        static_cast<void>(ours_handle_close(bootstrap));
        if (status != ours::Status::Ok) {
            fail("no processargs message on the bootstrap channel: ", status);
        }

        status = ProcessArgs::parse({message, num_bytes}, num_handles, &startup_args);
        if (status != ours::Status::Ok) {
            fail("the processargs message is malformed: ", status);
        }

        // A copy of the handle_infos the table can clear as handles are
        // taken; the message itself stays as it arrived.
        for (u32 i = 0; i < num_handles; ++i) {
            infos[i] = startup_args.handle_info(i);
        }
        startup_handles = StartupHandles({handles, num_handles}, {infos, num_handles});
    }
} // namespace

auto take_startup_handle(ours::HandleType const type, u16 const arg) -> ours::HandleValue {
    return startup_handles.take(type, arg);
}

auto process_args() -> ProcessArgs const & {
    return startup_args;
}

auto vdso_base() -> ours::VirtAddr {
    return vdso;
}
} // namespace runtime

/// Reads what the process was started with, calls main, and ends the process
/// with main's return value. Never returns: there is nothing to return to.
///
/// A process started without a bootstrap channel (HANDLE_INVALID in %rdi,
/// which design 3.4 allows) gets no handles and argc 0, and argv is still a
/// valid array with its terminating nullptr.
///
/// argv lives on this frame, which outlives main because it never returns;
/// its strings are the message's own, NUL-terminated where they lie. Sized
/// by the message: at most one pointer per byte of it, 32 KiB for a
/// pathological 4 KiB message of empty strings, well inside the 64 KiB
/// stack. Zircon's old __libc_start_main kept argv the same way, in a
/// variable-length array on the start-up frame.
extern "C" [[noreturn]]
auto runtime_start(ours::HandleValue const bootstrap, ours::VirtAddr const vdso_base) -> void {
    runtime::vdso = vdso_base;
    if (bootstrap != ours::HANDLE_INVALID) {
        runtime::read_processargs(bootstrap);
    }

    auto const args = runtime::startup_args.args();
    auto **const argv =
        static_cast<char **>(__builtin_alloca(sizeof(char *) * (usize{args.size()} + 1)));
    auto **slot = argv;
    for (auto const arg: args) {
        // |message| is not const, so handing its bytes out as char * is
        // sound; C's argv is char ** by definition.
        *slot++ = const_cast<char *>(arg.data());
    }
    *slot = nullptr;

    ours_process_exit(runtime_call_main(static_cast<int>(args.size()), argv));
}
