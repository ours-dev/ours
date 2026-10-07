/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// What runs between the entry point (crt0.S) and main, and after main.
#include <ort/debug.hpp>
#include <ort/process_args.hpp>
#include <ort/startup.hpp>
#include <ort/startup_handles.hpp>

#include <string_view>

#include <ours/compiler.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

/// crt0.S: a tail jump to main, which C++ may not call itself.
extern "C" auto ort_call_main(int argc, char **argv) -> int;

extern "C" [[noreturn]]
auto ort_start(ours::Uandle bootstrap, ours::VirtAddr vdso_base) -> void;

/// What runs between reading the processargs message and main, for a library
/// that has to act on the startup handles first: fsio, which binds the
/// namespace from the NsDir handles, so that main can open files at once.
///
/// This one is empty and weak, and fsio's definition replaces it in every
/// program that links fsio. A weak definition rather than a weak reference:
/// a reference nobody defines would stay in the program as an undefined
/// dynamic symbol, which the loaders' binder, which binds only to the vDSO,
/// cannot resolve. The runtime cannot call fsio by name: fsio is built on it.
extern "C" WEAK_LINK
auto ort_extensions_init() -> void {}

namespace ort {
namespace {
    // Everything the process was started with, for as long as it runs:
    // argv's strings and every string process_args() hands out point into
    // |MESSAGE|, so it cannot live in a frame that returns.
    //
    // Static and constant-initialised (constinit), because nothing runs a
    // program's dynamic initialisers: there is no .init_array walk, here or
    // in the loader. A global that needed a constructor would silently start
    // out as zeros.
    //
    // A channel message is at most 4 KiB and 64 handles, so fixed buffers
    // hold any bootstrap message, and the read needs no size query before it.
    alignas(ours::ProcArgs) constinit u8 MESSAGE[ours::CHANNEL_MAX_MSG_BYTES] = {};
    constinit ours::Uandle HANDLES[ours::CHANNEL_MAX_MSG_HANDLES] = {};
    constinit u32 INFOS[ours::CHANNEL_MAX_MSG_HANDLES] = {};
    constinit ProcessArgs STARTUP_ARGS;
    constinit StartupHandles STARTUP_HANDLES;
    constinit ours::VirtAddr VDSO = 0;

    // The process's handle to itself (HandleType::ProcSelf), taken from the
    // startup handles before main and kept for the life of the process.
    constinit ours::Uandle PROCESS_SELF = ours::Uandle::Invalid;

    [[noreturn]]
    auto fail(std::string_view const what, ours::Status const status) -> void {
        auto _ = ort::debug_line({"ort: ", what, std::string_view{ours::to_string(status)}});
        ours::process_exit(EXIT_START_FAILED);
    }

    /// Reads the processargs message off |bootstrap| into the buffers above,
    /// checks it, and closes |bootstrap|.
    ///
    /// Waits for the message first, because the starter may have started this
    /// process before sending its bootstrap message. Every starter here does
    /// exactly that: the message carries the child's root VMA, which only
    /// exists once the child does, so the kernel, userboot, init and the
    /// shell all write it after the spawn -- and a child that read before the
    /// write landed, on another CPU, would find nothing. The wait ends on
    /// READABLE, or on PEER_CLOSED if the starter drops its end without
    /// writing, and then the read reports PeerClosed rather than hanging; only
    /// a starter that keeps its end and never writes leaves this waiting,
    /// which is that starter's bug.
    ///
    /// Then one read, which must find the message: a message that does not
    /// fit is bigger than any channel message can be.
    ///
    /// The channel is closed once read, whatever came of it: the message was
    /// its only purpose, and a program that needs to talk to its starter is
    /// handed a channel for that among its startup handles. Its peer sees
    /// PEER_CLOSED as soon as the runtime has read the message, before main
    /// runs.
    auto read_processargs(ours::Uandle const bootstrap) -> void {
        auto observed = ours::Signals::None;
        auto _ =
            ours::object_wait_one(bootstrap, ours::Signals::Readable | ours::Signals::PeerClosed,
                                  ours::INSTANT_INFINITE, &observed);

        u32 num_bytes = 0;
        u32 num_handles = 0;
        auto status = ours::channel_read(bootstrap, ours::ChannelReadOptions::None, MESSAGE,
                                         HANDLES, sizeof(MESSAGE), ours::CHANNEL_MAX_MSG_HANDLES,
                                         &num_bytes, &num_handles);
        auto _ = ours::handle_close(bootstrap);
        if (status != ours::Status::Ok) {
            fail("no processargs message on the bootstrap channel: ", status);
        }

        status = ProcessArgs::parse({MESSAGE, num_bytes}, num_handles, &STARTUP_ARGS);
        if (status != ours::Status::Ok) {
            fail("the processargs message is malformed: ", status);
        }

        // A copy of the handle_infos the table can clear as handles are
        // taken; the message itself stays as it arrived.
        for (u32 i = 0; i < num_handles; ++i) {
            INFOS[i] = STARTUP_ARGS.handle_info(i);
        }
        STARTUP_HANDLES = StartupHandles({HANDLES, num_handles}, {INFOS, num_handles});

        // Taken here, before main or a library could take it: the runtime
        // owns it, and every caller of process_self() borrows the same one.
        PROCESS_SELF = STARTUP_HANDLES.take(ours::HandleType::ProcSelf, 0);
    }
} // namespace

auto take_startup_handle(ours::HandleType const type, u16 const arg) -> ours::Uandle {
    return STARTUP_HANDLES.take(type, arg);
}

auto process_args() -> ProcessArgs const & {
    return STARTUP_ARGS;
}

auto vdso_base() -> ours::VirtAddr {
    return VDSO;
}

auto process_self() -> ours::Uandle {
    return PROCESS_SELF;
}
} // namespace ort

/// Reads what the process was started with, calls main, and ends the process
/// with main's return value. Never returns: there is nothing to return to.
///
/// A process started without a bootstrap channel (Uandle::Invalid in %rdi,
/// which design 3.4 allows) gets no handles and argc 0, and argv is still a
/// valid array with its terminating nullptr.
///
/// argv lives on this frame, which outlives main because it never returns;
/// its strings are the message's own, NUL-terminated where they lie. Sized
/// by the message: at most one pointer per byte of it, 32 KiB for a
/// pathological 4 KiB message of empty strings, well inside the 64 KiB
/// stack.
extern "C" [[noreturn]]
auto ort_start(ours::Uandle const bootstrap, ours::VirtAddr const vdso_base) -> void {
    ort::VDSO = vdso_base;
    if (bootstrap != ours::Uandle::Invalid) {
        ort::read_processargs(bootstrap);
    }
    ort_extensions_init();

    auto const args = ort::STARTUP_ARGS.args();
    auto **const argv =
        static_cast<char **>(__builtin_alloca(sizeof(char *) * (usize{args.size()} + 1)));
    auto **slot = argv;
    for (auto const arg: args) {
        // |MESSAGE| is not const, so handing its bytes out as char * is
        // sound; C's argv is char ** by definition.
        *slot++ = const_cast<char *>(arg.data());
    }
    *slot = nullptr;

    ours::process_exit(ort_call_main(static_cast<int>(args.size()), argv));
}
