/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <logz4/log.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
auto debug_write(user_copy::UserInPtr<char const> const buffer, usize const length) -> Status {
    if (length == 0) {
        return Status::Ok;
    }
    if (length > MAX_DEBUG_WRITE) {
        return Status::OutOfRange;
    }

    // Into a kernel buffer first, and only then to the logger. It used to be
    // the other way round -- a range check, then the user pointer handed
    // straight to Logger::write() -- which meant the first touch of user
    // memory happened inside the logger with its lock held. A copy can fault;
    // on a SMAP CPU that one did, and the fault handler's own log line waited
    // on the lock this CPU already held, so the machine hung with nothing
    // printed. The rule is general: a plain copy, which resolves its own
    // fault, is never made under a spin lock or a lock the fault path takes;
    // a copy that has to run under one uses the *_capture_faults variants
    // (user_copy/user_copy.hpp).
    //
    // MAX_DEBUG_WRITE bounds the buffer, which is what makes a stack array
    // the right place for it.
    char text[MAX_DEBUG_WRITE];
    if (auto const status = buffer.copy_array_from_user(text, length); status != Status::Ok) {
        return status;
    }

    // Straight to the sink, not through log::info().
    //
    // Passing the bytes as a *format argument* already stopped a user program
    // directing the formatter with its own data -- but the formatter is still
    // the wrong machine for this. std::format assembles into an output buffer
    // that heap-allocates once the line outgrows its inline array, and this
    // kernel has no global operator new, only a panic stub
    // (kernel/lib/libc/cpp_dependencies.cpp:16). So a long enough write
    // panicked the kernel from ring 3:
    //
    //     [cpu1 error] operator new
    //
    // userboot's thirty-byte message never came close; //examples/shell asked
    // to print a 250-byte command list and took the kernel down with it.
    //
    // Logger::write() is the sink's own entry point -- logz4 hands it a fully
    // assembled line and it does no formatting at all, which is exactly what
    // bytes that are already bytes need. It also means the output is the user's
    // and nothing else: no level prefix, no timestamp wrapped around a fragment
    // of a line the program intends to continue.
    log::get_global_logger().write(text, length);
    return Status::Ok;
}
} // namespace ours::syscall
